# Slint port, phase 1: the inventory walked row by row

Every row of `inventory/main-window-frame.txt` and `inventory/spectrum-waterfall.txt` (FoxSDR 0.99.104, the design's source of truth) against the Slint main screen (`src/slint/ui/main.slint`, `src/slint/main.cpp`). **PRESENT** = built and wired; **RELOCATED** = moved as the Workbench IA says (a relocation, not a loss); **DEFERRED** = not in phase 1, with the reason. Companion screens, the plugin kit and the 3D wells are phases 3+. Updated with each phase.


## Main window frame — Window layout (how the main window is built)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1 | Cabinet | Brass frame, bevel, four screws, sunk well around everything | RELOCATED | no cabinet on Workbench: the window ground, hairlines and panels replace the brass frame (BRIEF: restyle allowed) |
| 2 | Title rail | Top strip of the cabinet; replaces the OS title bar (Windows only) | PRESENT | the 36 px custom title bar on every platform (main.slint title-bar) |
| 3 | Deck (toolbar) | Directly under the rail, full width | RELOCATED | the 104 px receiver strip |
| 4 | FUNCTION SELECT rail | Left column under the deck | RELOCATED | the 64 px icon rail + the 296 px sections panel |
| 5 | Centre | Between rail and status column: spectrum, waterfall, pages (, aw.cpp:13463) - inventoried elsewhere. Replaced by the Patch canvas when the PATCH view key is pressed (, aw.cpp:4761) | PRESENT | tab row + spectrum + splitter + waterfall; the PATCH tab is the PatchPage (rows 1888 to 1963), the SCOPE tab is the radar scope (docs 6.57) |
| 6 | STATUS column | Right column under the deck | RELOCATED | the 28 px status bar of seven chips; since 6.62 a chip squeezed by a long language gives up its detail first (the detail elides, the tip says it) so the right-hand chips and the two keys stay inside the window |
| 7 | Scope mode | Replaces rail + centre + status column; the deck stays. Entered from a radar/instrument;  leaves it when nothing is being typed and no popup is open (, aw.cpp:20136, Esc at ~20160) - inventoried elsewhere | PRESENT | the SCOPE tab holds the radar scope (RadarScopeView, docs 6.57); it fills the centre column, the rail and the status bar stay |
| 8 | Floating windows | Plugin windows, pages, markers, import dialog etc. are separate top-level ImGui windows drawn over the root (, aw.cpp:21884; dialogs list at aw.cpp:4770-4782) | RELOCATED | pages and plugin windows dock as tabs or tear off (Tab, TornWindow, WindowChrome built; phase-3 content) |

## Main window frame — Title rail (window chrome, Windows only)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 9 | Window title | readout | PRESENT | brand + version in the title bar (main.slint) |
| 10 | Minimise key | button | PRESENT | icon keys with tooltips and accessible labels; the host minimises / maximises-restores / quits |
| 11 | Maximise / restore key | button | PRESENT | icon keys with tooltips and accessible labels; the host minimises / maximises-restores / quits |
| 12 | Close key | button | PRESENT | icon keys with tooltips and accessible labels; the host minimises / maximises-restores / quits |
| 13 | Rail drag handle | drag/gesture | PRESENT | press-drag the empty bar moves the window (title-drag -> set_position); double-click toggles maximise |
| 14 | Rail system menu | context menu | DEFERRED | Windows' own system menu needs the native handle, which the Slint C++ API does not expose: phase 2 (callback system-menu-requested exists) |
| 15 | Window edges and corners | drag/gesture | PRESENT | right, bottom and corner edges drag-resize (resize-drag -> set_size); left and top edges follow in phase 2 |

## Main window frame — STOP / START dome and MASTER lamps

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 16 | Transport dome | button | PRESENT | StopKey (STOP while running, START stopped), tooltip, Ctrl+F2 -> FOXAPI_OP_RUN |
| 17 | RUN lamp | lamp/indicator | PRESENT | Lamp green while running |
| 18 | DEC lamp | lamp/indicator | PRESENT | Lamp green on FOXAPI_RX_DECODER_ACTIVE |
| 19 | MUTE lamp | lamp/indicator | PRESENT | Lamp amber on the user mute or a plugin mute (audioMutedBy) |
| 20 | FAIL lamp | lamp/indicator | PRESENT | Lamp red on FOXAPI_RX_FAULTED |

## Main window frame — BIAS TEE key

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 21 | BIAS TEE key | key (latched toggle) with an amber lamp in its centre | PRESENT | hazard Key BIAS TEE with its lamp; off->on opens the question sheet, on->off at once; disabled with the hint when the radio has none |
| 22 | BIAS TEE tooltip | tooltip | PRESENT | the 4.5 V warning as the key's tooltip and in the question sheet |

## Main window frame — TUNED - HERTZ counter

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 23 | Plate name | readout | RELOCATED | the strip's caption 'Tuned frequency' names the counter |
| 24 | Figure cell (x10) | number field / drag-gesture | PRESENT | ten Digits: wheel steps the digit, click opens the typed editor, leading zeros dim |
| 25 | Hover hint | tooltip | PRESENT | the counter's tooltip |
| 26 | Up switch (x10) | button (auto-repeat) | PRESENT | nudge arrows with auto-repeat (400 ms then 80 ms); hidden by 'Show tuner switches' |
| 27 | Down switch (x10) | button (auto-repeat) | PRESENT | nudge arrows with auto-repeat (400 ms then 80 ms); hidden by 'Show tuner switches' |
| 28 | RCVR lamp | lamp/indicator | PRESENT | amber Lamp lit while the receiver runs, left of the MHz readout |
| 29 | MHz readout | readout | PRESENT | mono readout in accent-soft above the counter |
| 30 | Unit scale | readout | PRESENT | unit row under the figures (FrequencyCounter) |
| 31 | Converter caption | readout | RELOCATED | the slot under the figures shows 'via <converter>' in warning colour when FOXAPI_LIST_CONVERTERS has an active one, else the sheet's tune hint; the engraving's decorative role is the title bar's maker mark |
| 32 | Counter right-click menu | context menu | PRESENT | right-click on the plate opens the counter menu (Menu) |

## Main window frame — Frequency editor (typed entry)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 33 | Frequency edit box | text field | PRESENT | TextInput over the figures: Enter commits (parseFrequencyHz -> SET_FREQUENCY), Esc / focus loss cancels; T opens it |

## Main window frame — Counter right-click menu

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 34 | Menu title | readout | PRESENT | menu heading |
| 35 | Enlarge figures | menu item (checked when on) | DEFERRED | the counter scale is the interface-size setting of phase 2; the items are in the menu and log their request |
| 36 | Normal size | menu item (checked when on) | DEFERRED | the counter scale is the interface-size setting of phase 2; the items are in the menu and log their request |
| 37 | Show tuner switches | menu item (checkmark) | PRESENT | toggles show-nudges |
| 38 | Counter face | menu item (submenu) | PRESENT | the faces as the skin's digit-face switch: Nixie tubes = nixie, Neon = drum, Plain = plain, applied through Theme.skin |
| 39 | Nixie tubes / Neon / Plain | menu item (radio-style check) | PRESENT | the faces as the skin's digit-face switch: Nixie tubes = nixie, Neon = drum, Plain = plain, applied through Theme.skin |
| 40 | Enlarge every reading | menu item (checkmark) | DEFERRED | the counter scale is the interface-size setting of phase 2; the items are in the menu and log their request |

## Main window frame — GAIN / SQUELCH / VOLUME dials

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 41 | Gain dial | dial/knob | RELOCATED | a slider lane (SliderGroup) on the strip with the readout and AUTO key on the caption row (Workbench IA 2); N/A with no stage, AUTO with AGC on, dimmed when it cannot be set |
| 42 | Gain tooltip | tooltip | PRESENT | the lane's tooltip, with the stage name or why no radio is open |
| 43 | Gain AUTO key | key (latched toggle) | PRESENT | xs AUTO key on the Gain lane -> SET_DEVICE_AGC; disabled with its reason as tooltip when the radio has no AGC |
| 44 | Squelch dial | dial/knob | RELOCATED | a slider lane -120..0 dB -> SET_SQUELCH; turning it leaves AUTO |
| 45 | Squelch tooltip | tooltip | PRESENT | the lane's tooltip |
| 46 | Squelch AUTO key | key (latched toggle) | PRESENT | xs AUTO key -> SET_SQUELCH -200 (the table's always-open / automatic); tooltip changes with the state |
| 47 | Volume dial | dial/knob | RELOCATED | a slider lane 0..1 -> SET_VOLUME; + / - keys |

## Main window frame — SAMPLE RATE and VOLUME meters

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 48 | Sample rate meter | meter | RELOCATED | the rate is a readout in the status bar's RECEIVER chip and the rail footer (Workbench IA 2); the Meter component with its four styles exists for instruments |
| 49 | Volume meter | meter | RELOCATED | the level bar under the volume slider, -60 dB floor (Workbench IA 2) |
| 50 | Meter right-click menu | context menu | DEFERRED | no deck meter on Workbench; the Meter component takes menu-requested for the instruments of phase 3 |
| 51 | Meter hover hint | tooltip | DEFERRED | no deck meter on Workbench; the Meter component takes menu-requested for the instruments of phase 3 |
| 52 | Classic | menu item (check) | PRESENT | the four styles of the Meter component (meters.slint); not on the main screen |
| 53 | Analogue needle | menu item (check) | PRESENT | the four styles of the Meter component (meters.slint); not on the main screen |
| 54 | LED ladder | menu item (check) | PRESENT | the four styles of the Meter component (meters.slint); not on the main screen |
| 55 | Peak meter | menu item (check) | PRESENT | the four styles of the Meter component (meters.slint); not on the main screen |

## Main window frame — Sound muted banner (deck)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 56 | Banner words | banner | PRESENT | Banner under the strip while audioMutedBy is set |
| 57 | Stop plugin | button (small) | PRESENT | the banner's key -> DECODER_STOP for each FOXAPI_LIST_MUTE_CAUSES entry |

## Main window frame — FUNCTION SELECT rail (left column)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 58 | Plate title | readout | RELOCATED | the status bar has no plate title |
| 59 | Rail fold key | button | PRESENT | the rail's FOLD key folds the sections panel to the icon rail (animated) |
| 60 | Rail unfold key | button | PRESENT | the same key reads SHOW when folded |
| 61 | Bank key SIGNAL | key (latched toggle, radio group; lamp strip beneath) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5; a caption longer than the 64 px rail elides and the tip says it first (6.62) |
| 62 | Bank key DECODE | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5; a caption longer than the 64 px rail elides and the tip says it first (6.62) |
| 63 | Bank key TOOLS | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5; a caption longer than the 64 px rail elides and the tip says it first (6.62) |
| 64 | Bank key EXTEND | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5; a caption longer than the 64 px rail elides and the tip says it first (6.62) |
| 65 | Bank key SYSTEM | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5; a caption longer than the 64 px rail elides and the tip says it first (6.62) |
| 66 | Bank group caption | readout | PRESENT | the panel's head caption |
| 67 | Bank curtain | readout (animation) | DEFERRED | the enamel fade is the ImGui window's; the panel width animates on fold instead (cosmetic) |
| 68 | View key RECEIVER | key (latched, 2-way) | RELOCATED | the RECEIVER tab of the tab row (Workbench IA 4) |
| 69 | View key PATCH | key (latched, 2-way) | RELOCATED | the PATCH tab; its page is PatchPage (patch_page.slint, rows 1888 to 1963) |
| 70 | Sections area | list | PRESENT | the bank's rows with state chips in a Flickable; the Radio body is drawn (passband, tune step, band-plan checkbox), the SIGNAL bank's bodies are drawn (phase 3); TOOLS, EXTEND and SYSTEM remain |
| 71 | Rail footer | readout | PRESENT | two-line footer: source · band (FOXAPI_LIST_BAND_PLAN), rate · channel · underruns; the converter line replaces line 2 in warning colour |
| 72 | Radio section mode keys | (not here) | RELOCATED | the eight mode keys are the strip's segmented control (Workbench IA 2) from 1760 px of window width; below that they fold into one picker key that opens the same eight as a menu (the sheet's strip does not fit 1440 by its own numbers); the Radio body says so; Ctrl+F .. Ctrl+R stay |

## Main window frame — Folded strips

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 73 | Folded rail strip | key | PRESENT | the icon rail is the folded strip: SHOW unfolds |
| 74 | Folded status strip | key | RELOCATED | the status bar never folds, so no unfold key is needed |

## Main window frame — STATUS column (right)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 75 | Plate title | readout | RELOCATED | the status bar has no plate title |
| 76 | Status fold key | button | RELOCATED | the status bar never folds |
| 77 | AUDIO - UNDERRUNS card | readout | PRESENT | UNDERRUNS chip: count, red when > 0, lines ring / plugin gaps on click |
| 78 | DECODER OUTPUT card | readout | PRESENT | OUTPUT chip: lines/s over a 2 s window, '--' with the reasons |
| 79 | DECODERS card | readout | PRESENT | DECODERS chip: running / ready, of n fitted, idle / not fed |
| 80 | SINK card | readout | PRESENT | SINK chip: OPEN / MUTED / OPENING / NO DEVICE / STOPPED with the lines |
| 81 | RECORDER card | readout | PRESENT | RECORDER chip from FOXAPI_LIST_RECORDINGS: off / IQ / AUDIO / IQ + AUDIO / FULL / STALLED |
| 82 | WEB ACCESS card | readout | PRESENT | WEB chip from FOXAPI_LIST_SERVERS: off / port n / REFUSED + reason |
| 83 | RECEIVER card | readout | PRESENT | RECEIVER chip with the lamp: RUNNING / STOPPED / FAULT, source, antenna, gain, rate (PPM when the table publishes it) |
| 84 | REQUEST A FEATURE | button (brass word key) | PRESENT | ghost key at the status bar's right end; the page is a companion screen (phase 3) |
| 85 | REPORT A BUG / DISLIKE | button (brass word key) | PRESENT | ghost key at the status bar's right end; the page is a companion screen (phase 3) |
| 86 | Maker plate | readout | RELOCATED | the maker mark in the title bar |

## Main window frame — Message from the FoxSDR team (banner)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 87 | Team message card | banner | PRESENT | Card at the panel foot with the Seen key; hidden until the table carries the message (API 0.3 has none) |
| 88 | Seen | button (full width) | PRESENT | Card at the panel foot with the Seen key; hidden until the table carries the message (API 0.3 has none) |

## Main window frame — Update available (banner)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 89 | Headline | banner | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 90 | Running version | readout | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 91 | Release notes | list | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 92 | Download update | button | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 93 | Download size note | readout | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 94 | Install now and restart | button | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 95 | Open foxsdr.com (Linux) | button | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 96 | Not now | button (small) | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 97 | Update error | readout | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |

## Main window frame — Microsoft Store update (banner)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 98 | Headline | banner | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 99 | Running version | readout | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 100 | Waiting notice | readout | DEFERRED | the Store's install flow has no table state in API 0.3; the same Card draws it in phase 2 |
| 101 | Outcome text | readout | DEFERRED | the Store's install flow has no table state in API 0.3; the same Card draws it in phase 2 |
| 102 | Install from the Microsoft Store | button (full width) | DEFERRED | the Store's install flow has no table state in API 0.3; the same Card draws it in phase 2 |
| 103 | Not now | button (small) | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |

## Main window frame — Sound is muted (dialog)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 104 | Message | dialog | DEFERRED | a companion dialog (phase 3); the banner path with 'Stop plugin' exists |
| 105 | Stop and resume | button | DEFERRED | a companion dialog (phase 3); the banner path with 'Stop plugin' exists |
| 106 | Keep it running | button | DEFERRED | a companion dialog (phase 3); the banner path with 'Stop plugin' exists |

## Main window frame — Turn on the bias tee? (dialog)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 107 | Question | dialog | PRESENT | the Sheet 'Turn on the bias tee?' with the radio's sentence and the warning |
| 108 | Turn it on | button | PRESENT | Key -> SET_BIAS_TEE 1 |
| 109 | Cancel | button | PRESENT | the lit (default) key closes the sheet |

## Main window frame — FoxSDR did not close normally last time (diagnostics offer)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 110 | Explanation | dialog | DEFERRED | a companion dialog (phase 3) |
| 111 | Close box (title X) | button | DEFERRED | a companion dialog (phase 3) |
| 112 | Copy diagnostics | button | DEFERRED | a companion dialog (phase 3) |
| 113 | Open reports folder | button | DEFERRED | a companion dialog (phase 3) |
| 114 | Not now | button | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |

## Main window frame — Link this FoxSDR? (beta tester link prompt)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 115 | Prompt text | dialog | DEFERRED | a companion dialog (phase 3) |
| 116 | Link | button | DEFERRED | a companion dialog (phase 3) |
| 117 | Not now | button | PRESENT | Card properties: headline (red when critical), 'you are running', notes as bullets, primary key or progress bar, note, error, Not now; hidden until the table carries updater state (none in API 0.3: phase 2) |
| 118 | Error variant | dialog | DEFERRED | a companion dialog (phase 3) |
| 119 | Close box | button | DEFERRED | a companion dialog (phase 3) |

## Main window frame — Shortcuts

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 120 | Ctrl+F2 | Start / stop the receiver (startStop) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 121 | M | Mute the sound (mute) - toggles user mute; lights the MUTE lamp | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 122 | + (the  key) / - | Volume up / down, 0.05 per press (volumeUp, volumeDown) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 123 | Ctrl+Shift+PageUp / PageDown | Squelch up / down, 2 dB per press, switches AUTO squelch off (squelchUp, squelchDown) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 124 | Ctrl+F | Mode: NFM (narrow FM) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 125 | Ctrl+Shift+F | Mode: WFM (broadcast FM) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 126 | Ctrl+A | Mode: AM | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 127 | Ctrl+D | Mode: DSB | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 128 | Ctrl+U | Mode: USB | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 129 | Ctrl+C | Mode: CW | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 130 | Ctrl+L | Mode: LSB | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 131 | Ctrl+R | Mode: RAW | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 132 | Ctrl+Up / Ctrl+Down | Tune up / down one step (the 10 kHz digit, cell 5) (tuneStepUp, tuneStepDown) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 133 | Ctrl+PageUp / Ctrl+PageDown | Tune up / down one screen width (visible span) (tuneSpanUp, tuneSpanDown) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 134 | T | Type a frequency - opens the counter's editor (quickTune) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 135 | Ctrl++ / Ctrl+- | Zoom the spectrum in / out about the centre (zoomIn, zoomOut) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 136 | Ctrl+Delete | Zoom back to the full span (zoomReset) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 137 | Shift+R | Record the audio - starts/stops the audio recording (record) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 138 | Space | Transmit (hold) (transmitPtt) | PRESENT | the PTT key's chord (row 1354): Space held while the PTT key has the keyboard focus (click it or Tab to it) - stricter than the ImGui window's 'while the page has focus'; the bound chord is read from the key table each press |
| 139 | Ctrl+W | Save a screenshot (screenshot) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 140 | F11 | Maximise / restore the window (fullscreen) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 141 | F1 / F2 / F3 / F4 / F5 | Rail banks SIGNAL PATH / DECODE / TOOLS / EXTEND / SYSTEM (bankSignal, bankDecode, bankView [sic, id kept for TOOLS], bankExtend, bankSystem) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 142 | F7 | Settings and key bindings - goes to SYSTEM and opens the Key bindings row (openSettings) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 143 | F12 | Always saves a screenshot (hard-wired, not in the table) | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 144 | Esc | Cancels the counter editor, the key-capture, a dialog; leaves Scope mode when nothing is being edited and no popup is open | PRESENT | the root FocusScope names the key and the host answers the SAVED binding by config id (key_dispatch, docs 6.56); the host acts |
| 145 | Ctrl+= / Ctrl+- / Ctrl+0 | Interface size: step up / step down through 100-250%, Ctrl+0 back to Auto. Not rebindable; ignored while text is being typed | DEFERRED | Ctrl+0 (back to Auto) is answered and applied: key-dispatch names uiSizeAuto, main.cpp saves interfaceScale = auto through SystemPublisher::setInterfaceSize and the window's look moves the scale on the next tick (docs 6.61, drive system-look.txt: pin 150%, Ctrl+0, scale 1.00). Ctrl+= and Ctrl+- are NOT the interface size here: the key table binds them to the band zoom (Ctrl+Plus / Ctrl+Minus), so stepping the size through 100-250% by the keyboard stays deferred; the dropdown steps it |
| 146 | Mouse wheel over a counter figure, a dial | Tunes that digit / turns the dial | PRESENT | wheel over a figure steps it; wheel over a slider moves it 5 % |

## Main window frame — Themes and sizes

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 147 | Today's bench () | The default brass-and-enamel bench; serif ("bench-serif") fonts, Nixie counter | PRESENT | Workbench is the Slint filling and what the saved key 'today' is drawn in (look_logic skinOfTheme, applied at start and on a pick; drive system-look.txt reads look.bg / look.accent); the Nixie counter face comes with it (tunerDisplayStyle nixie). The serif fonts of the classic are not part of Workbench (Inter) |
| 148 | Bench Classic XL () | Today's palette, counter figures doubled, switches put away, every reading 1.4x | DEFERRED | drawn in Workbench with the Nixie face (docs 6.61); the doubled counter figures, the put-away switches and the 1.4x readings are saved but not applied (rows 35, 36, 40, 156-158: the counter scale has no home in the Slint counter yet) |
| 149 | Night Watch () | Dark red-black palette with red sheen/highlights (preserves night vision); mono fonts, flat keys | PRESENT | the ONE ported skin besides Workbench (docs 6.61): theme.slint NightWatch, the classic's makeNight palette mapped role by role (red-only; the reading pairs 4.5:1, test_slint_theme), the mono typeface, flat keys (2 px radius, no shadow), neon counter face; installed with one assignment (Theme.skin) at start and when 'night' is picked, no component changed (test_slint_main_screen renders the whole screen in it, drive system-look.txt reads look.bg #070303) |
| 150 | Glass Cockpit () | Dark blue-grey; sans UI font, condensed captions, chrome knobs, radius 8 | DEFERRED | drawn in Workbench's colours with the plain counter face (docs 6.61: look_logic skinOfTheme names Workbench for every key but 'night'); the dark blue-grey skin with its sans fonts and radius 8 is one more NightWatch-style value of the Skin struct, not yet made |
| 151 | Daylight Lab () | Light grey/white; sans fonts, chrome knobs, radius 6 | DEFERRED | drawn in Workbench's colours with the plain counter face (docs 6.61: look_logic skinOfTheme names Workbench for every key but 'night'); the light skin is one more Skin value, not yet made |
| 152 | Field Radio () | Olive/khaki; mono body with stencil captions | DEFERRED | drawn in Workbench's colours with the plain counter face (docs 6.61: look_logic skinOfTheme names Workbench for every key but 'night'); the olive skin is one more Skin value, not yet made |

## Main window frame — Themes and sizes: the Display section's settings

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 153 | Interface size | dropdown "Interface size" | PRESENT | the Display body's DropKey (row 1388); the window's scale follows it live (docs 6.61) |
| 154 | Theme | dropdown "Theme" | PRESENT | the Display body's DropKey (row 1390); the window's skin and counter face follow it live (docs 6.61) |
| 155 | Frequency display | dropdown "Frequency display" | PRESENT | the counter menu's Counter face |
| 156 | Counter figures | dropdown "Counter figures" | DEFERRED | the SYSTEM > Display body is a phase-3 bank |
| 157 | Tuner switches | checkbox "Tuner switches" | PRESENT | the counter menu's Show tuner switches |
| 158 | Enlarge every reading | checkbox | DEFERRED | the counter scale is the interface-size setting of phase 2; the items are in the menu and log their request |
| 159 | Min dB / Max dB | slider x2 | PRESENT | the Display block's Floor / Ceiling sliders in the trace and marker menus |
| 160 | Aircraft icons | slider | PRESENT | the slider is the SYSTEM Display body's (row 1397, saved as aircraftIconPx); the value is applied in the map: main.cpp hands slintmap::mapLookOf(interface half: aircraftIconPx, mapTrailWidthPx, mapTrailStyle) to KitPublisher::setMapLook every tick; kit_publish.cpp fillMap passes it to TileCompositor::setTrailLook and sets MapTarget.size of every aircraft; map.slint MapPane draws the icon, its rings, hit area and label at that size: 16..96 px across, 48 by default (gui/aircraft_icons.hpp bounds, copied in map_compositor.hpp and read against the source by test_slint_host_map) |
| 161 | Trail width | slider () | PRESENT | the slider is the SYSTEM Display body's (row 1398, saved as mapTrailWidthPx); the value is applied in the map: main.cpp hands slintmap::mapLookOf(interface half: aircraftIconPx, mapTrailWidthPx, mapTrailStyle) to KitPublisher::setMapLook every tick; kit_publish.cpp fillMap passes it to TileCompositor::setTrailLook and sets MapTarget.size of every aircraft; map.slint MapPane draws the icon, its rings, hit area and label at that size (TileCompositor strokes the trails at the width, half-width = width / 2): 1 px up to the wingspan of the icon in force, 86 px at 96 |
| 162 | Meter style (x2) | right-click menu on each deck meter | DEFERRED | no deck meter on Workbench |
| 163 | Band plan | checkbox + size/colour/region combos | PRESENT | the toolbar's Band plan key; size / colour / region are the Display body of phase 3 |

## Spectrum and waterfall — Spectrum panel (top of the centre of the Receiver view)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 164 | Header title | readout | PRESENT | SpectrumView caption 'Spectrum · n bin' (+ zoom) |
| 165 | Average caveat | readout | PRESENT | SpectrumView caveat |
| 166 | Peak caption | readout | PRESENT | 'Peak in passband' cap, shown with a peak |
| 167 | Peak figure | readout | PRESENT | mono 16 px figure from the bins inside the passband |
| 168 | Data age | readout | PRESENT | 'now' / 'n.n s ago' from the last frame's stamp |
| 169 | Trace | overlay/marker | PRESENT | host-painted image (paintTrace) filling the plot |
| 170 | dB ladder | readout | PRESENT | labels at each dB tick, thinned to 20 dB under a 40 px pitch |
| 171 | Horizontal gridlines | overlay/marker | PRESENT | a graticule line per dB tick |
| 172 | Frequency axis | readout | PRESENT | nice 1-2-5 ticks about 80 px apart, labels in MHz with the unit at the right |
| 173 | Vertical tick gridlines | overlay/marker | PRESENT | a full-height graticule line per frequency tick |
| 174 | SPAN box | readout | PRESENT | mono Chip at the top right |
| 175 | VFO band (shaded) | overlay/marker | PRESENT | shaded band with edges and a dashed centre; brighter while dragged |
| 176 | Band-plan ribbon reserve | overlay/marker | PRESENT | the ribbon along the top of the well, sized 1..3 by ribbon-size |
| 177 | Peak-hold / average overlay trace | overlay/marker | PRESENT | bold peak-hold (orange) or average (ivory) trace painted over the live one |
| 178 | Trace-mode label | readout | PRESENT | 9 px cap under the header in the trace's colour |
| 179 | Grab-and-click interaction zone | drag/gesture | PRESENT | one TouchArea over the plot |

## Spectrum and waterfall — Waterfall panel (bottom of the centre of the Receiver view)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 180 | Waterfall picture | readout | PRESENT | host-scrolled picture (scrollWaterfallRow), newest row at the top |
| 181 | Elapsed-time strip (gutter) | readout | PRESENT | 'Age' cap and ticks from the measured publish rate |
| 182 | Reception-gap break | overlay/marker | DEFERRED | the component takes gap-pos; the host's gap detection from frame stamps is phase 2 |
| 183 | Strength key plate | readout | PRESENT | the plate with title, ramp, labels and floor/ceiling line |
| 184 | Key: manual flag | lamp/indicator | PRESENT | warn chip MANUAL while Range: Manual |
| 185 | Key: floor/ceiling | readout | PRESENT | 'FLOOR x dB · CEILING y dB · auto/manual' |
| 186 | Key: coverage | readout | DEFERRED | the component takes key-coverage; the host does not yet track which rows a range change left behind (phase 2) |
| 187 | Range-changed rail | overlay/marker | DEFERRED | the component takes rail-pos; host tracking of the change row is phase 2 |
| 188 | Foot line 1: scroll | readout | PRESENT | 'n lines/s · n s of history' from the measured rate |
| 189 | Foot line 2: decode/receive status | readout | PRESENT | tuned frequency in accent-soft · mode · decoding: <fed decoder> / receiving / STOPPED |
| 190 | VFO line (tuned frequency) | overlay/marker | PRESENT | 1 px line at the tuned frequency |
| 191 | Splitter | drag/gesture | PRESENT | Splitter: drag rebalances split-ratio 0.1..0.9; grip lights on hover |

## Spectrum and waterfall — Mouse and keyboard gestures on the spectrum

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 192 | Click (off the band) | drag/gesture | PRESENT | FOXAPP_OP_VFO_TO_ABSOLUTE, snap unless Shift |
| 193 | Drag the shaded band | drag/gesture | PRESENT | SET_FREQUENCY by the dragged fraction of the view |
| 194 | Drag a band edge | drag/gesture | PRESENT | FOXAPP_OP_SET_BANDWIDTH_DRAG, 6 px hit zone, ew-resize cursor |
| 195 | Double-click empty spectrum | drag/gesture | PRESENT | back to the full span |
| 196 | Right-click | drag/gesture | PRESENT | the marker menu (Menu) at the pointer |
| 197 | Wheel | drag/gesture | PRESENT | zoom 1.3x per notch about the pointer, floor sample rate / 1000 |
| 198 | Shift + wheel | drag/gesture | PRESENT | STEP_TUNE one mode step per notch |
| 199 | Tooltip over band centre | tooltip | PRESENT | the view's tooltip strings by where the pointer rests |
| 200 | Tooltip over band edge | tooltip | PRESENT | the view's tooltip strings by where the pointer rests |
| 201 | Tooltip off the band | tooltip | PRESENT | the view's tooltip strings by where the pointer rests |
| 202 | Rest tip (4 s) | tooltip | DEFERRED | the 4 s rest tip is phase-2 polish |
| 203 | Zoom the spectrum in | key (shortcut) | PRESENT | Ctrl++ / Ctrl+- / Ctrl+Delete and the toolbar's zoom keys |
| 204 | Zoom the spectrum out | key (shortcut) | PRESENT | Ctrl++ / Ctrl+- / Ctrl+Delete and the toolbar's zoom keys |
| 205 | Zoom back to the full span | key (shortcut) | PRESENT | Ctrl++ / Ctrl+- / Ctrl+Delete and the toolbar's zoom keys |

## Spectrum and waterfall — Mouse gestures on the waterfall

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 206 | Click (press and release without moving) | drag/gesture | PRESENT | a press under 450 ms and 4 px tunes on release |
| 207 | Drag horizontally | drag/gesture | PRESENT | pans the zoomed view after 4 px (panBy) |
| 208 | Double-click | drag/gesture | PRESENT | back to the full span |
| 209 | Right-click | drag/gesture | PRESENT | the marker menu (Menu) at the pointer |
| 210 | Wheel / Shift+wheel | drag/gesture | PRESENT | as the spectrum |
| 211 | Tooltip (normal) | tooltip | PRESENT | the waterfall tooltip |
| 212 | Tooltip (Airband listening) | tooltip | DEFERRED | the Airband monitor is a phase-3 plugin-kit feature |
| 213 | Rest tip (4 s) | tooltip | DEFERRED | the 4 s rest tip is phase-2 polish |
| 214 | Marker tooltip | tooltip | PRESENT | marker-hovered within 4 px of a line or over its tab shows M<n> + note |

## Spectrum and waterfall — Trace-mode menu (right-click on the spectrum)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 215 | Normal | menu item | PRESENT | checked items -> trace-mode |
| 216 | Peak hold | menu item | PRESENT | checked items -> trace-mode |
| 217 | Average | menu item | PRESENT | checked items -> trace-mode |
| 218 | (separator) |  | PRESENT | menu separator |
| 219 | Reset trace | menu item | PRESENT | resets the hold; disabled in Normal |
| 220 | Average length | submenu | PRESENT | flattened into the same menu: a heading, the presets and the slider |
| 221 | 100 ms ... 10 s presets | menu item | PRESENT | radio items -> averageLengthS |
| 222 | Average length slider | slider | PRESENT | logarithmic slider item 50 ms .. 10 s |
| 223 | Display block | (see "Display block") | PRESENT | the Display block at the foot of both menus |

## Spectrum and waterfall — Marker menu (right-click on the waterfall)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 224 | Drop marker here | menu item | PRESENT | FOXAPP_OP_MARKER_DROP at the pointer frequency (merge reach 2 px) |
| 225 | Remove marker M<n> | menu item | PRESENT | FOXAPP_OP_MARKER_REMOVE when a marker is within 6 px |
| 226 | (separator) |  | PRESENT | menu separator |
| 227 | List markers... | menu item | DEFERRED | the Markers window is a companion window (phase 3; the Table component exists); the item logs |
| 228 | Copy markers to clipboard | menu item | DEFERRED | the text is built; the clipboard needs a platform call the C++ API lacks in 1.18 (phase 2) |
| 229 | Clear all markers | submenu | PRESENT | opens the are-you-sure menu |
| 230 | Yes, remove all N | menu item | PRESENT | FOXAPP_OP_MARKER_CLEAR |
| 231 | Display block | (see below) | PRESENT | the Display block at the foot of both menus |

## Spectrum and waterfall — Display block (end of both right-click menus)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 232 | Section heading | readout | PRESENT | menu heading |
| 233 | Range: Automatic | radio | PRESENT | radio items and the toolbar's Range keys |
| 234 | Range: Manual | radio | PRESENT | radio items and the toolbar's Range keys |
| 235 | Range tooltip | tooltip | PRESENT | tooltips on the toolbar's Range keys |
| 236 | Floor | slider | PRESENT | slider item -> FOXAPP_OP_SET_DISPLAY_MIN, enabled with Manual |
| 237 | Ceiling | slider | PRESENT | slider item -> FOXAPP_OP_SET_DISPLAY_MAX, enabled with Manual |
| 238 | Reset display to defaults | menu item | PRESENT | Automatic, -110..0, trace Normal, holds cleared |

## Spectrum and waterfall — Markers window (List markers...)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 239 | Window | dialog | DEFERRED | the Markers window is a companion window (phase 3) |
| 240 | Error line | readout | DEFERRED | the Markers window is a companion window (phase 3) |
| 241 | Empty message | readout | DEFERRED | the Markers window is a companion window (phase 3) |
| 242 | Marker table | table | DEFERRED | the Markers window is a companion window (phase 3) |
| 243 | M<n> cell | readout | DEFERRED | the Markers window is a companion window (phase 3) |
| 244 | Frequency cell | readout | DEFERRED | the Markers window is a companion window (phase 3) |
| 245 | Noted cell | readout | DEFERRED | the Markers window is a companion window (phase 3) |
| 246 | Note field | text field | DEFERRED | the Markers window is a companion window (phase 3) |
| 247 | Tune | button | DEFERRED | the Markers window is a companion window (phase 3) |
| 248 | Bookmark | button | DEFERRED | the Markers window is a companion window (phase 3) |
| 249 | x | button | DEFERRED | the Markers window is a companion window (phase 3) |
| 250 | Copy markers to clipboard | button | DEFERRED | the text is built; the clipboard needs a platform call the C++ API lacks in 1.18 (phase 2) |
| 251 | Clear all markers | button | PRESENT | opens the are-you-sure menu |
| 252 | Yes, remove all N | menu item | PRESENT | FOXAPP_OP_MARKER_CLEAR |

## Spectrum and waterfall — Waterfall markers drawn on the plots

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 253 | Marker line (waterfall) | overlay/marker | PRESENT | dashed 60 % line down the picture |
| 254 | Marker tab | overlay/marker | PRESENT | mono Chip tab at the foot, stacking up to 3 rows |
| 255 | Marker line (spectrum) | overlay/marker | PRESENT | dashed 35 % line through the trace |
| 256 | Marker tooltip | tooltip | PRESENT | marker-hovered within 4 px of a line or over its tab shows M<n> + note |

## Spectrum and waterfall — Overlays on the spectrum from other features

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 257 | Band-plan ribbon | overlay/marker | PRESENT | FOXAPI_LIST_BAND_PLAN segments in the view |
| 258 | Band-plan palette | overlay/marker | DEFERRED | Classic / Vivid / Mono palettes are the Display body's (phase 3); the ribbon draws Theme.bandplan |
| 259 | Bookmark ticks | overlay/marker | PRESENT | 2 x 8 ticks above the axis, favourites with a guide line |
| 260 | Bookmark names | overlay/marker | PRESENT | names under the header (one row; stacking rows with the Bookmarks setting in phase 3) |
| 261 | Plugin mark: point | overlay/marker | PRESENT | FOXAPI_LIST_MARKERS lines in the mark's colour, dashed when flagged |
| 262 | Plugin mark: span | overlay/marker | PRESENT | tinted band with edges |
| 263 | Plugin marks on waterfall | overlay/marker | PRESENT | the same lines down the picture unless SPECTRUM_ONLY |
| 264 | Airband channel marks | overlay/marker | DEFERRED | the Airband monitor is a phase-3 plugin-kit feature |
| 265 | Airband caption: block | readout | DEFERRED | the Airband monitor is a phase-3 plugin-kit feature |
| 266 | Airband caption: hearing | readout | DEFERRED | the Airband monitor is a phase-3 plugin-kit feature |
| 267 | Airband monitor effect on plots | lamp/indicator | DEFERRED | the Airband monitor is a phase-3 plugin-kit feature |

## Spectrum and waterfall — Settings that change what the centre draws (live elsewhere)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 268 | Band plan | checkbox | PRESENT | the toolbar's Band plan key; size / colour / region are the Display body of phase 3 |
| 269 | Size | dropdown/combo | DEFERRED | the Display and Bookmarks section bodies are phase-3 banks |
| 270 | Colour | dropdown/combo | DEFERRED | the Display and Bookmarks section bodies are phase-3 banks |
| 271 | Plan status text | readout | DEFERRED | the Display and Bookmarks section bodies are phase-3 banks |
| 272 | Min dB | slider | PRESENT | the Display block's sliders |
| 273 | Max dB | slider | PRESENT | the Display block's sliders |
| 274 | On the spectrum | checkbox | DEFERRED | the Display and Bookmarks section bodies are phase-3 banks |
| 275 | Stack close names | checkbox | DEFERRED | the Display and Bookmarks section bodies are phase-3 banks |

# Phase 2: the plugin kit (docs 6.38) - the five plugin tabs walked row by row

Every row of the five plugin tabs of `inventory/` (`decode-bank-plugin-host.txt`, `plugins-aircraft-marine.txt`, `plugins-broadcast-text.txt`, `plugins-paging-meters-tools.txt`, `plugins-satellites-weather-maps.txt`; FoxSDR 0.99.104) against the Slint plugin kit: `src/slint/ui/pages.slint`, the families (`presets`, `forms`, `output`, `table`, `fitted`, `instruments`, `map`) and the kit host (`kit_publish.cpp`, `plugin_kit.cpp`, `kit_logic.cpp`). The status words are phase 1's: **PRESENT** = the kit draws it (a plugin's config-file keys and environment variables are PRESENT as not window controls), **RELOCATED** = a window that became a docked or torn-off page, or another form of the same control, **DEFERRED** = not in the kit yet, with the reason.

## DECODE bank and plugin host — DECODE group caption

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 276 | Group caption | readout | PRESENT | the panel's head caption (row 66) |
| 277 | DECODE bank key | key (latched toggle) | PRESENT | RailKey DECODE with the F2 hint (row 62) |

## DECODE bank and plugin host — Plugin store (rail row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 278 | Plugin store switch row | key (latched toggle) | PRESENT | the Plugin store row in DecodeBank, drawn as a chip LATER; it opens nothing until the store (a later beta) |
| 279 | Store chip + lamp | readout / lamp | DEFERRED | the store's BUSY / IDLE / n UPD chip: the store is a later beta (scope locked 2026-11-02) |
| 280 | Row tooltip | tooltip | DEFERRED | the store row's tooltip sentence: the row is a chip until the store |
| 281 | Catalogue index.json URL | text field | PRESENT | a Field in DecodeBank, committed on Enter or focus lost (FOXAPP_OP_SET_CATALOGUE_URL) |
| 282 | Enforcement error | readout | PRESENT | the enforcement error line under the field, in the error tone |

## DECODE bank and plugin host — Disabled (retired plugins), under the store row

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 283 | Disabled heading | readout | PRESENT | the Card titled Disabled (an error card) |
| 284 | Block message | readout | PRESENT | the card's line: the whole message, never cut |
| 285 | Update | button | PRESENT | the card's primary key when a catalogue id exists; with no catalogue plan the fetch waits for the store (a later beta) |
| 286 | Fetch hint | readout | PRESENT | the card's note when the card has no Update key |
| 287 | Remove | button (small) | PRESENT | the card's Remove key, first step of the two-step delete |
| 288 | Delete confirmation | readout + button x2 | PRESENT | Confirm delete / Cancel on the card; the armed state expires after 5 s |

## DECODE bank and plugin host — Plugins (rail row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 289 | Plugins switch row | key (latched toggle) | PRESENT | a Section row 'Plugins' (with the disabled count when retired files exist); it opens the Fitted modules page |
| 290 | Plugins chip + lamp | readout / lamp | PRESENT | the row's chip (fed/decoders) and state dot |
| 291 | Row tooltip | tooltip | PRESENT | the row's tooltip, the ImGui sentence |
| 292 | Refusal/not-fed note | readout | PRESENT | the refused / not-fed note under the row, error or warning tone |

## DECODE bank and plugin host — Turn on and off plugins (rail drawer; internal id "decoders")

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 293 | Section header | context-free drawer key | PRESENT | the 'Turn on and off plugins' drawer: a Section with the chip 'n FED' |
| 294 | Decoder absence / status note | readout | PRESENT | the census sentence at the top of the drawer |
| 295 | Show/Hide decoder output | button | PRESENT | the Show / Hide decoder output key in the drawer |
| 296 | Lines decoded | readout | PRESENT | the 'n lines decoded' line |
| 297 | Log tail note | readout | PRESENT | the 500-line tail note |
| 298 | Running cost note | readout | PRESENT | the running-cost note, warning tone |
| 299 | Stop all | button | PRESENT | the STOP ALL n RUNNING key (danger tone) with the ImGui tooltip |
| 300 | Category heading | readout | PRESENT | a Caption over each group of entries (the store-category headings) |
| 301 | Plugin entry heading | readout | PRESENT | the entry's name line, one block per plugin |
| 302 | Catalogue preset button | button | PRESENT | a key of the entry's PresetBar (column form), lit when the receiver is on that preset |
| 303 | User preset button | button | PRESENT | an operator's preset key in the same PresetBar |
| 304 | Forget preset | button | PRESENT | the x key on an operator's preset (a right-click forgets too); the green note follows |
| 305 | Save preset | button | PRESENT | the PresetBar's '+ Save <MHz> as a preset' key |
| 306 | Start/Stop key | button | PRESENT | RunKeys: STOP / START with the three tooltips |
| 307 | Mute audio while running | checkbox | PRESENT | RunKeys: the Mute audio while running checkbox (per-plugin override) |
| 308 | Plugin settings fold | tree node | PRESENT | PluginSettingsForm in the entry (the Plugin settings fold) |
| 309 | Empty-list note | readout | PRESENT | the empty-list note under the entries |
| 310 | Preset result note | readout | PRESENT | the green result note under the PresetBar |
| 311 | Receiver control (stale grants) | checkbox list | PRESENT | ReceiverControlCard: the refusal sentence and the stale-grant checkboxes |

## DECODE bank and plugin host — Target details (rail drawer)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 312 | Section header | drawer key | PRESENT | the Target details drawer: a Section with the id chip and a state dot |
| 313 | Empty hint | readout | PRESENT | TargetDetails' empty hint |
| 314 | Gone note | readout | PRESENT | TargetDetails' gone note |
| 315 | Detail lines | readout | PRESENT | TargetDetails' detail lines under the heading |
| 316 | Go to on map | button (small) | PRESENT | TargetDetails' Go to on map key |
| 317 | Follow / Stop following | button (small) | PRESENT | TargetDetails' Follow / Stop following key |

## DECODE bank and plugin host — Map page rows (one per track-source plugin)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 318 | Map page switch row | key (latched toggle) | PRESENT | a Section per map page ('<plugin> map', 'n TGT', lamp = open); a click also sends FOXAPP_OP_PLUGIN_AUTO_PRESET |
| 319 | No-tracker row | switch row (blocked) | PRESENT | the blocked 'Target maps NONE' row and its note |

## DECODE bank and plugin host — Plugin window rows (one per picture, panel or instrument)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 320 | Image row | key (latched toggle) | PRESENT | a Section per picture ('<plugin> image', IDLE / WAIT / RX / IMG) with the shared tooltip |
| 321 | Panel row | key (latched toggle) | PRESENT | a Section per panel (title, IDLE / 'n ROW') with the shared tooltip |
| 322 | Instrument row | key (latched toggle) | PRESENT | a Section per instrument (title, IDLE / NEW / WAIT / kind word) with the shared tooltip |

## DECODE bank and plugin host — Window title rail and frame (every page)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 323 | Title | readout | PRESENT | the tab's title and the torn window's title strip |
| 324 | Minimise key | button | RELOCATED | the torn window's Roll up / Restore key (a docked tab has none) |
| 325 | Maximise/Restore key | button | RELOCATED | the torn window's Maximise key, which docks the page back as a tab |
| 326 | Close key | button | PRESENT | the tab's close key and the torn window's Close |
| 327 | Title-rail drag | drag/gesture | RELOCATED | drag down from a tab tears it off; a torn window moves by its title strip (an OS-level window is a later step) |
| 328 | Title-rail double-click | drag/gesture | RELOCATED | double-click on a torn window's title strip docks it back as a tab |
| 329 | Resize grip | drag/gesture | RELOCATED | the torn window's resize grip |
| 330 | Reset window sizes | see Fitted modules | RELOCATED | see row 107: it puts the torn windows' rectangles and the panels' column widths back; no rectangle is saved |

## DECODE bank and plugin host — Decoder output (window; rail key "Show decoder output")

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 331 | Window | window | RELOCATED | the Decoder output page: a docked tab or a torn window (WindowChrome / TornWindow) |
| 332 | Follow | checkbox | PRESENT | the Follow checkbox on the Decoder output page |
| 333 | Clear | button (small) | PRESENT | the Clear key |
| 334 | Line count | readout | PRESENT | the line count |
| 335 | Preset bars | key set | PRESENT | one PresetBar per running text decoder that has presets |
| 336 | Log | list | PRESENT | LogView: the plugin name dim before each line, WARN amber, ERROR red, a virtual list; 'Listening...' when empty |

## DECODE bank and plugin host — Picture window ("<PLUGIN> IMAGE")

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 337 | Window | window | RELOCATED | the picture page: a docked tab or a torn window |
| 338 | Preset bar | key set | PRESENT | PresetBar at the top of the picture page |
| 339 | Plugin settings fold | tree node | PRESENT | the Plugin settings fold on the picture page (PluginSettingsForm) |
| 340 | Waiting note | readout | PRESENT | PicturePane's 'Waiting for the first image...' |
| 341 | Image status | readout | PRESENT | PicturePane's status line ('WxH  complete / receiving...  frame n') |
| 342 | Save as BMP | button (small) | PRESENT | PicturePane's Save as BMP key (synchronous) |
| 343 | Save note | readout | PRESENT | the save note beside the key, which says the path |
| 344 | Picture | readout (clickable) | PRESENT | PicturePane fits the picture; a click on an interactive picture sends FOXAPP_OP_IMAGE_PRESS in picture pixels |

## DECODE bank and plugin host — Panel window (plugin's own table)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 345 | Window | window | RELOCATED | the panel page: a docked tab or a torn window |
| 346 | Preset bar, Plugin settings fold | key set / tree node | PRESENT | PresetBar and the settings fold on the panel page |
| 347 | Rows table | table | PRESENT | Table with the plugin's headings: resizable columns (drag a heading edge), a virtual body of 2000 rows, 'Nothing to show yet.' |
| 348 | Row cell colour | readout | PRESENT | Table tones: WARN amber, GOOD green, MUTED dim |
| 349 | Pressable row | key (row) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |

## DECODE bank and plugin host — Instrument window

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 350 | Window | window | RELOCATED | the instrument page: a docked tab or a torn window |
| 351 | Preset bar, Plugin settings fold | key set / tree node | PRESENT | PresetBar and the settings fold on the instrument page |
| 352 | Face | readout | PRESENT | InstrumentFace draws the face of the kind (nine faces) |
| 353 | Memory table | table | PRESENT | the memory table beneath the face (Table; pressable rows allowed) |
| 354 | Fax chart panel | readout (clickable) | DEFERRED | the chart is drawn in the FaxFace paper slot, not beside the memory table; the full-size picture is the picture page |
| 355 | NEW hold | lamp | DEFERRED | the NEW lamp follows the unread seq and clears when the page draws; the 4 s hold is not kept |

## DECODE bank and plugin host — Map page (generic, non-satellite) - brief, map agent owns the detail

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 356 | Preset bar / Plugin settings | key set / tree node | PRESENT | PresetBar and the settings fold on the map page |
| 357 | Fit | button (small) | PRESENT | the Fit key in MapToolbar (the satellites page's own deck, WHOLE WORLD included, is deferred) |
| 358 | RX position entry | text field x2 + button | RELOCATED | Set RX here arms the next click on the map (FOXAPI_OP_SET_POSITION) in place of two lat/lon boxes; the RX readout shows the position |
| 359 | Target count | readout | PRESENT | the target count ('n targets, m heard without a position') |
| 360 | On screen only | checkbox | PRESENT | the On screen only checkbox |
| 361 | Coverage | checkbox | PRESENT | the Coverage checkbox |
| 362 | Trails | checkbox | PRESENT | the Trails checkbox |
| 363 | Altitude colours | checkbox | PRESENT | the Altitude colours checkbox, disabled without Trails |
| 364 | Trail style | dropdown/combo | PRESENT | the SYSTEM Display body's Trail style box (Line / Ribbon, under Trail width; docs 6.52) saves mapTrailStyle (0 line, 1 ribbon), which the map applies - main.cpp -> KitPublisher::setMapLook -> fillMap -> TileCompositor::setTrailLook (docs 6.44); the kit map page itself has no menu for it (row 1713) |
| 365 | Trail width | slider | PRESENT | the saved width (mapTrailWidthPx) is applied by TileCompositor::setTrailLook in map_compositor.cpp (main.cpp -> KitPublisher::setMapLook -> kit_publish.cpp fillMap), half-width = width / 2, bounded 1 .. the wingspan of the icon in force (clampTrailWidthPx); the slider itself is the SYSTEM Display body's (row 1398) |
| 366 | Reset coverage | button (small) | PRESENT | the Reset coverage key |
| 367 | Coverage status | readout | PRESENT | the coverage status sentence |
| 368 | GPS position | text field, combo, buttons | DEFERRED | the GPS position row: the serial-port page is a System bank page (later) |
| 369 | Target list | table | PRESENT | the target list (Callsign / ID / Note) with the Sort key and the Up / Down key; click selects and centres, double-click follows; the hover tip shows callsign, altitude and speed only |
| 370 | Details button | button (small) | RELOCATED | a click on a list row selects the target for the Target details drawer; there is no Details key |
| 371 | Stop following | button (small) | PRESENT | the following note and the Stop following key |
| 372 | Stop following? dialog | dialog | DEFERRED | a drag while following is refused instead of asked; Stop following is the key |

## DECODE bank and plugin host — Target details (window)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 373 | Window | window | RELOCATED | the Target details drawer on the DECODE bank, and the same body as a page |
| 374 | Body | readout | PRESENT | TargetDetails body |
| 375 | Go to on map | button | PRESENT | TargetDetails' Go to on map key |
| 376 | Follow / Stop following | button | PRESENT | TargetDetails' Follow / Stop following key |
| 377 | Close | button | PRESENT | TargetDetails' Close key on the page form |

## DECODE bank and plugin host — Fitted modules (window; rail key "Plugins"): top bar and list

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 378 | Window | window | RELOCATED | the Fitted modules page: a docked tab (an OS-level window is a later step) |
| 379 | Search | text field | PRESENT | the Search fitted modules box |
| 380 | State chips x6 | toggle | PRESENT | StateChips: six chips with counts and lamps, each toggles the filter |
| 381 | SCAN AGAIN | button | PRESENT | the SCAN AGAIN key (FOXAPI_OP_PLUGIN_RESCAN) |
| 382 | RESET WINDOW SIZES | button | PRESENT | the RESET WINDOW SIZES key: every page back to its default rectangle |
| 383 | Verdict line | readout | PRESENT | the verdict line with the folder |
| 384 | Install report / error | readout | PRESENT | the install report / error line |
| 385 | Group heading | readout | PRESENT | the category headings over the rows |
| 386 | Module row | list | PRESENT | FittedRowLine: a click opens the module page, a double-click opens the module's own window |
| 387 | State word + lamp | lamp/indicator | PRESENT | the row's state word and dot |
| 388 | STOP / START | button | PRESENT | STOP / START on the row |
| 389 | REMOVE | button | PRESENT | REMOVE / REMOVE FILE on the row, first step of the two-step delete |
| 390 | CONFIRM | button | PRESENT | CONFIRM within 5 s |
| 391 | Cleanup foot | button + dialog | DEFERRED | CLEAN UP OLD VERSIONS (n) and its dialog: the store is a later beta (the foot is in FittedModules; the host feeds it no label) |
| 392 | Empty-list text | readout | PRESENT | the empty-list sentences |

## DECODE bank and plugin host — Module page (click a name)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 393 | Back | button | PRESENT | the < FITTED back key in ModulePage |
| 394 | Header | readout | PRESENT | ModulePage header: name, maker and version |
| 395 | STOP / START | button | PRESENT | STOP / START at the right of the header |
| 396 | State lamp + word | lamp/indicator | PRESENT | the state lamp and word |
| 397 | WHAT THIS MODULE REACHES | readout | PRESENT | WHAT THIS MODULE REACHES: the capability rows (ReachRow) and the lead / note sentences |
| 398 | GRANT / REVOKE RECEIVER CONTROL | button | PRESENT | GrantKeys: GRANT / REVOKE RECEIVER CONTROL with its note |
| 399 | GRANT / REVOKE RADIO SETTINGS | button | PRESENT | GrantKeys: GRANT / REVOKE RADIO SETTINGS with its note |
| 400 | GRANT / REVOKE TRANSMIT | button | PRESENT | GrantKeys: GRANT / REVOKE TRANSMIT with the transmit plate sentence |
| 401 | Plugin command keys | button | PRESENT | PlateKeys: the plugin's command keys two per row |
| 402 | State sentence | readout | PRESENT | the state sentence (WHY IT IS NOT RUNNING for a refused file) |
| 403 | Keep running | checkbox | PRESENT | the Keep running checkbox |
| 404 | Notes | readout | PRESENT | the orphan / integrity / origin / notice lines |
| 405 | FILE / PLUGIN ABI / fitted date | readout | PRESENT | FILE, PLUGIN ABI and the fitted date (FactRow) |
| 406 | Catalogue body | readout/key | DEFERRED | the catalogue body (SCREENSHOTS, WHAT IT DOES, WHAT'S NEW, DETAILS): the store is a later beta |

## DECODE bank and plugin host — Preset bar (window-side preset keys)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 407 | Preset key | button (small) | PRESENT | a key of PresetBar, lit when the receiver is on the preset |
| 408 | + Save | button (small) | PRESENT | the + Save key of PresetBar, with the ImGui tooltip |
| 409 | Forget | context menu (right-click) | PRESENT | a right-click on an operator's preset forgets it (the x key too) |
| 410 | Separator | readout | PRESENT | the bar's hairline rule under it |

## DECODE bank and plugin host — Sound muted dialog and banner (generic plugin mute behaviour)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 411 | Sound is muted | dialog | PRESENT | the 'Sound is muted' sheet on MUTE_TUNE_AWAY: 'Stop <names> and resume sound' / 'Keep it running' (FOXAPP_OP_MUTE_KEEP_RUNNING) |
| 412 | Mute banner | readout + button | PRESENT | the Banner under the strip with the Stop plugin key (FOXAPP_OP_DECODER_STOP_LIST) |

## DECODE bank and plugin host — Instrument faces: generic face (kind 0 or unknown)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 413 | Plate | readout | PRESENT | GenericFace plate with the instrument title |
| 414 | Lamps ALERT, NEW, LOCK, BATT | lamp/indicator | PRESENT | Lamps ALERT (blinks), NEW (from the unread seq), LOCK, BATT |
| 415 | Text slots | readout | PRESENT | GenericFace lists every filled text slot on glass |
| 416 | Value slots | readout | PRESENT | GenericFace lists every non-zero value slot |
| 417 | Event counter | readout | PRESENT | the EVENT n counter |
| 418 | Rail chip | readout | PRESENT | the instrument row's chip in the DECODE bank |

## DECODE bank and plugin host — Instrument faces: pager face (kind 1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 419 | LCD | readout | PRESENT | PagerFace LCD (PaleLcd): the latest page wrapped at 20 columns, the time strip, the envelope with the unread count |
| 420 | Case plate | readout | PRESENT | the case plate legend |
| 421 | Lamps | lamp/indicator | PRESENT | the ALERT, NEW, SYNC, BATT lamps |
| 422 | Drums | readout | PRESENT | DrumCells CAPCODE (7) and UNREAD (2) |
| 423 | Wells | readout | PRESENT | the TYPE and RECEIVED wells |
| 424 | Footer | readout | PRESENT | the footer: EVENT n or NO PAGE RECEIVED |
| 425 | Controls | - | PRESENT | no keys on the face, by the house rule |

## DECODE bank and plugin host — Instrument faces: teleprinter face (kind 2)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 426 | Paper strip | readout | DEFERRED | PaperStrip prints the plugin's text slots; the older lines of the roll are not fed by the host yet |
| 427 | STATUS group | lamp/indicator | PRESENT | the STATUS lamps PAPER, READY, NEW, ALERT (ALERT blinks) |
| 428 | MESSAGES | readout | PRESENT | the MESSAGES drums |
| 429 | Controls | - | PRESENT | no scroll or tear key, by the house rule |

## DECODE bank and plugin host — Instrument faces: tone-alert face (kind 3)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 430 | Station lamp | lamp/indicator | PRESENT | the station BigLamp (ALERT) |
| 431 | Tone bars | meter | PRESENT | ReadBar TONE A / TONE B on the Hz scale; an empty well with no tone |
| 432 | Cells | readout | PRESENT | the cells CODE, FORMAT, RESULT and the A/B seconds |
| 433 | Plate | readout | PRESENT | the plate legend |
| 434 | Footer | readout | PRESENT | the footer EVENT n and the NEW lamp |
| 435 | Controls | - | PRESENT | no keys; the call log is the memory table beneath the face |

## DECODE bank and plugin host — Instrument faces: VOR (nav bearing) face (kind 4)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 436 | OBS knob | dial/knob | PRESENT | ObsKnob, the face's only control: drag in a circle, wheel, arrow keys |
| 437 | Compass card | readout | PRESENT | the Compass card turned to the radial |
| 438 | Deviation bar + dots | meter | PRESENT | CentreZeroMeter: the deviation bar, 2 degrees a dot |
| 439 | TO / FROM | lamp/indicator | PRESENT | the TO / FROM lamps |
| 440 | NAV flag | lamp/indicator | PRESENT | BarberFlag NAV |
| 441 | Readouts | readout | PRESENT | the readouts RADIAL FROM, IDENT, CONF, REF, VAR, SIGNAL (blank when there is no reading) |
| 442 | Lamps | lamp/indicator | PRESENT | the LOCK and NEW lamps |
| 443 | Rail chip | readout | PRESENT | the instrument row's chip: a 3-digit bearing when locked, else NAV |

## DECODE bank and plugin host — Instrument faces: radiofax recorder face (kind 5)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 444 | Phase lamps | lamp/indicator | PRESENT | the phase lamps IDLE, START, PHASING, PICTURE (plus NEW, LOCK) |
| 445 | IOC / LPM selectors | readout | PRESENT | the IOC and LPM selectors |
| 446 | Line counter | meter | PRESENT | the LINES drums |
| 447 | Tuning meter | meter | PRESENT | the TUNING CentreZeroMeter |
| 448 | Paper slot | readout | PRESENT | PaperSlot: the chart prints on the paper strip (the full-size picture is the picture page) |
| 449 | Controls | - | PRESENT | no keys on the face, by the house rule; a click on an interactive picture is the picture page's |

## DECODE bank and plugin host — Instrument faces: beacon face (kind 6)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 450 | DISTRESS lamp | lamp/indicator | PRESENT | the DISTRESS BigLamp, blinking while a burst is fresh |
| 451 | Identity | readout | PRESENT | the 15 hex ID on seven-segment digits |
| 452 | Fields | readout | PRESENT | the COUNTRY, PROTOCOL, POSITION plates |
| 453 | Carrier meter | meter | PRESENT | the CARRIER ERROR CentreZeroMeter |
| 454 | Counters | readout | PRESENT | the SINCE LAST BURST and BURSTS LOGGED counters |
| 455 | Lamp | lamp/indicator | PRESENT | the NEW lamp |
| 456 | Rail chip | readout | PRESENT | the instrument row's chip: ALERT, else the age, else 'n LOG' |

## DECODE bank and plugin host — Instrument faces: utility-meter face (kind 7)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 457 | Plate | readout | PRESENT | MeterFace plate, named by commodity |
| 458 | Register LCD | readout | PRESENT | PaleLcd register with UNITS |
| 459 | Lamps | lamp/indicator | PRESENT | the lamps ELECTRIC, GAS, WATER, ALERT, NEW, TAMPER (alarm tone) |
| 460 | Labels | readout | PRESENT | the labels METER No., COMMODITY, LAST HEARD |
| 461 | Rail chip | readout | PRESENT | the instrument row's chip: WAIT, MTR, 'n MTR' |

## DECODE bank and plugin host — Instrument faces: weather-console face (kind 8)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 462 | Channel compartments | readout | PRESENT | WeatherFace channel compartments (LCD digits, dashes for a channel not heard) |
| 463 | Lamps | lamp/indicator | PRESENT | the BATT and NEW lamps |
| 464 | Footer | readout | PRESENT | the footer: NO SENSOR HEARD YET and the READINGS memory title |
| 465 | Rail chip | readout | PRESENT | the instrument row's chip: NO RX, the temperature, 'n CH' |

## DECODE bank and plugin host — Plugin UI building blocks

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 466 | Settings form (CAP_SETTINGS_UI, up to 16 fields) | building block | PRESENT | PluginSettingsForm: TEXT, NUMBER, BOOL, PATH (as a text box) and PASSWORD; the BROWSE FILES dialog is deferred (it needs the native handle) |
| 467 | Preset bar / presets | building block | PRESENT | PresetBar on the plugin's page and the rail entry; the operator's presets with the x forget key and + Save |
| 468 | Tune controls (receiver control grants) | building block | PRESENT | GrantKeys on the module page and ReceiverControlCard in the drawer |
| 469 | Status rows | building block | PRESENT | the census note, the FED chip, the state word per module and the quoted reason on the module page |
| 470 | Panel rows | building block | PRESENT | Table rows on the panel page and under an instrument face; KEY rows are pressable |
| 471 | Keys on your plate (add_command, up to 16) | building block | PRESENT | PlateKeys on the module page, two per row |
| 472 | Messages (host log) | building block | PRESENT | the Decoder output page, the plugin name dim before each line; the newest WARN / ERROR on the module page note |
| 473 | Marks (set_marker, up to 64) | building block | PRESENT | marks on the spectrum and the waterfall: the main screen draws MARKERS (phase 1) |
| 474 | Instrument faces | building block | PRESENT | InstrumentFace with the nine faces, the instrument row's chip and the page chrome |
| 475 | Map tracks (track source) | building block | PRESENT | MapPane and the target list on the map page; the '<plugin> map' row with 'n TGT'; TargetDetails; coverage; unplaced targets listed with the unplaced note |
| 476 | Image decoder | building block | PRESENT | PicturePane with the preset bar, the settings fold, the size / frame line, Save as BMP and the clickable picture; the picture row chip |
| 477 | Text decoder | building block | PRESENT | the Decoder output page with the plugin tag per line |
| 478 | Target details | building block | PRESENT | TargetDetails in the drawer and as a page body |
| 479 | Plugin states | building block | PRESENT | the DECODE bank's Disabled cards, the REFUSED state word and WHY IT IS NOT RUNNING, the STOPPED chip and START key, the IDLE rows that wake the plugin |

## Plugins: aircraft and marine — ADS-B Aircraft Decoder (adsb-decoder 1.9.1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 480 | ADS-B preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 481 | Decoder output lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 482 | Position line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 483 | Surface line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 484 | Velocity lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 485 | Ident line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 486 | Squawk / emergency lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 487 | Other lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 488 | Hint / warning lines | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 489 | Frame log | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 490 | Replay | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 491 | Beast feed | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 492 | Aircraft target | map track | PRESENT | a target on the map page (MapPane: an aircraft icon, labelled, with a row in the target list) |
| 493 | Aircraft silhouette | map track (flag bits) | DEFERRED | MapPane draws one icon per kind (an aircraft icon); the category silhouettes are not ported |
| 494 | Emergency flag | map track flag | PRESENT | the emergency ring: a red halo that blinks (MapPane) |
| 495 | Heard without a position | target list / detail | PRESENT | the unplaced note and the 'heard without a position' count in the target list |
| 496 | Estimated position | map track flag | PRESENT | the estimated ring: dashed (MapPane) |
| 497 | Flight trail | map path | PRESENT | a Trails line on the map page (MapPane), faded with its target |
| 498 | Target details | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |

## Plugins: aircraft and marine — Aircraft Info (registry lookup) (aircraft-info 1.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 499 | Registration | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |
| 500 | Type code | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |
| 501 | Type name | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |
| 502 | Manufacturer | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |
| 503 | Operator | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |
| 504 | Country | target-detail field | PRESENT | a line of TargetDetails (the drawer and the page body) |
| 505 | Lookup scope | behaviour | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |
| 506 | foxsdr.com down | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 507 | adsbdb error | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 508 | Base URL override | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 509 | Base URL override | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 510 | Network reach | permission note | PRESENT | WHAT THIS MODULE REACHES on the module page (the amber line) |

## Plugins: aircraft and marine — avdb = "Airports & Navaids" (NOT in the catalogue; source version 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 511 | Airport target | map track | PRESENT | a target on the map page (MapPane: a station icon, labelled, with a row in the target list) |
| 512 | Navaid target | map track | PRESENT | a target on the map page (MapPane: an icon by kind, labelled, with a row in the target list) |
| 513 | Age | behaviour | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |
| 514 | Home position | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 515 | Radius | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 516 | Cache dir | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 517 | No fetch | env var (not UI) | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |

## Plugins: aircraft and marine — ACARS Aircraft Datalink Decoder (acars-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 518 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 519 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 520 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 521 | Preset 4 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 522 | Preset 5 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 523 | Preset 6 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 524 | Decoded message | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 525 | Instrument face | instrument face | PRESENT | the TeleprinterFace on the instrument page |
| 526 | Registration | readout (text[0]) | PRESENT | a slot of TeleprinterFace (text[0]) |
| 527 | Flight | readout (text[1]) | PRESENT | a slot of TeleprinterFace (text[1]) |
| 528 | Label | readout (text[2]) | PRESENT | a slot of TeleprinterFace (text[2]) |
| 529 | Mode | readout (text[3]) | PRESENT | a slot of TeleprinterFace (text[3]) |
| 530 | Block | readout (text[4]) | PRESENT | a slot of TeleprinterFace (text[4]) |
| 531 | Printed count | readout (values[0]) | PRESENT | a slot of TeleprinterFace (values[0]) |
| 532 | Memory col 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 533 | Memory col 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 534 | Memory col 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 535 | Memory col 4 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 536 | Memory col 5 | column | PRESENT | a column of the memory table beneath the face (Table) |

## Plugins: aircraft and marine — VDL Mode 2 (vdl2-decoder 0.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 537 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 538 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 539 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 540 | Preset 4 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 541 | Preset 5 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 542 | Preset 6 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 543 | Preset 7 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 544 | Preset 8 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 545 | Preset 9 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 546 | Preset 10 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 547 | Preset 11 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 548 | Banner | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 549 | Retune line | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 550 | Refusal line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 551 | Drop warning | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 552 | Frame lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 553 | Instrument face | instrument face | PRESENT | the TeleprinterFace on the instrument page |
| 554 | Face readouts | readout | PRESENT | a slot of TeleprinterFace |
| 555 | Memory columns | column | PRESENT | a column of the memory table beneath the face (Table) |
| 556 | Ground-station target | map track | PRESENT | a target on the map page (MapPane: a station icon, labelled, with a row in the target list) |
| 557 | Aircraft target | map track | PRESENT | a target on the map page (MapPane: an aircraft icon, labelled, with a row in the target list) |
| 558 | Not decoded | note | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |

## Plugins: aircraft and marine — HFDL Aircraft Datalink Decoder (hfdl-decoder 0.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 559 | 12 presets | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 560 | Preamble-not-recognised line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 561 | Odd-tone line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 562 | Bad-format line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 563 | Burst prefix | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 564 | ACARS line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 565 | Log-on lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 566 | Squitter line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 567 | Undecoded / refused lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 568 | Position paper line | message list / paper | PRESENT | the Decoder output page (text out); the roll of paper is TeleprinterFace |
| 569 | Instrument face | instrument face | PRESENT | the TeleprinterFace on the instrument page |
| 570 | Face readouts | readout | PRESENT | a slot of TeleprinterFace |
| 571 | Memory columns | column | PRESENT | a column of the memory table beneath the face (Table) |
| 572 | Aircraft target | map track | PRESENT | a target on the map page (MapPane: an aircraft icon, labelled, with a row in the target list) |

## Plugins: aircraft and marine — SELCAL Decoder (selcal-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 573 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 574 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 575 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 576 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 577 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 578 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 579 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 580 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 581 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 582 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 583 | Good call | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 584 | Doubtful call | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: aircraft and marine — VOR Navigation Decoder (vor-decoder 1.0.1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 585 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 586 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 587 | Bearing line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 588 | Ident line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 589 | No-subcarrier hint | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 590 | Instrument face | instrument face | PRESENT | the NavBearingFace on the instrument page |
| 591 | Bearing | readout (values[0]) | PRESENT | a slot of NavBearingFace (values[0]) |
| 592 | Confidence | readout (values[1]) | PRESENT | a slot of NavBearingFace (values[1]) |
| 593 | Reference level | readout (values[2]) | PRESENT | a slot of NavBearingFace (values[2]) |
| 594 | Variable level | readout (values[3]) | PRESENT | a slot of NavBearingFace (values[3]) |
| 595 | Ident | readout (text[0]) | PRESENT | a slot of NavBearingFace (text[0]) |
| 596 | OBS knob | button | PRESENT | ObsKnob, the face's only control, on NavBearingFace |

## Plugins: aircraft and marine — AIS Ship Tracking Decoder (ais-decoder 1.0.2)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 597 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 598 | NMEA sentence | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 599 | Class A position (types 1, 2, 3) | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 600 | Static voyage (type 5) | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 601 | Class B (type 18) | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 602 | Class B extended (type 19) | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 603 | Other types | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 604 | Channel status | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 605 | Vessel target | map track | PRESENT | a target on the map page (MapPane: a vessel icon, labelled, with a row in the target list) |

## Plugins: aircraft and marine — Inmarsat-C / EGC Decoder (stdc-decoder 0.1.1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 606 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 607 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 608 | Sync line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 609 | Frame error | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 610 | EGC header | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 611 | EGC text | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 612 | Debug line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: aircraft and marine — 406 MHz Distress Beacon Decoder (EPIRB / ELT / PLB) (epirb-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 613 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 614 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 615 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 616 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 617 | Distress line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 618 | Test line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 619 | Protocol texts | message list / readout | PRESENT | the Decoder output page (text out); the protocol plate is BeaconFace |
| 620 | Instrument face | instrument face | PRESENT | the BeaconFace on the instrument page |
| 621 | Hex ID | readout (text[0]) | PRESENT | a slot of BeaconFace (text[0]) |
| 622 | Country | readout (text[1]) | PRESENT | a slot of BeaconFace (text[1]) |
| 623 | Protocol | readout (text[2]) | PRESENT | a slot of BeaconFace (text[2]) |
| 624 | Position | readout (text[3]) | PRESENT | a slot of BeaconFace (text[3]) |
| 625 | Carrier error | readout (values[0]) | PRESENT | a slot of BeaconFace (values[0]) |
| 626 | Age | readout (values[1]) | PRESENT | a slot of BeaconFace (values[1]) |
| 627 | Burst log col 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 628 | Burst log col 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 629 | Burst log col 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 630 | Burst log col 4 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 631 | Safety wording | description text | DEFERRED | the store's catalogue body is a later beta (the plugin's red banner line is in the Decoder output page) |

## Plugins: aircraft and marine — NAVTEX & DSC (marine) (navtex-dsc-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 632 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 633 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 634 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 635 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 636 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 637 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 638 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 639 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 640 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 641 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 642 | NAVTEX message | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 643 | DSC call | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 644 | DSC refused | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 645 | Tones-swapped hint | status row | PRESENT | a line of the Decoder output page (the plugin's own state line) |
| 646 | Instrument face | instrument face | PRESENT | the TeleprinterFace on the instrument page |
| 647 | Station | readout (text[0]) | PRESENT | a slot of TeleprinterFace (text[0]) |
| 648 | What | readout (text[1]) | PRESENT | a slot of TeleprinterFace (text[1]) |
| 649 | Id | readout (text[2]) | PRESENT | a slot of TeleprinterFace (text[2]) |
| 650 | Mode | readout (text[3]) | PRESENT | a slot of TeleprinterFace (text[3]) |
| 651 | Errors | readout (text[4]) | PRESENT | a slot of TeleprinterFace (text[4]) |
| 652 | Printed | readout (values[0]) | PRESENT | a slot of TeleprinterFace (values[0]) |
| 653 | Paper col 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 654 | Paper col 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 655 | Vessel target | map track | PRESENT | a target on the map page (MapPane: a vessel icon, labelled, with a row in the target list) |
| 656 | Not covered | note | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |

## Plugins: broadcast and text — SSTV Image Decoder (sstv-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 657 | Picture | picture | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 658 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 659 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 660 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 661 | Preset 4 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 662 | Preset 5 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 663 | Start line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 664 | Mode found | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 665 | Unknown VIS | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 666 | Bad parity | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 667 | Progress | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 668 | Done | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: broadcast and text — Morse (CW) Decoder (cw-decoder 1.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 669 | Decoded text | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 670 | Speed line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 671 | Overflow line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 672 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 673 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 674 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 675 | Preset 4 | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 676 | Preset 5 | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |
| 677 | Preset 6 | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |

## Plugins: broadcast and text — RTTY Teleprinter Decoder (rtty-decoder 1.2.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 678 | Baud | setting (number) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 679 | Shift | setting (number) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 680 | Reverse | setting (bool) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 681 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 682 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 683 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 684 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 685 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 686 | Intro row | panel row (heading) | PRESENT | a Table row of the plugin's panel page |
| 687 | Baud row | panel row (key) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |
| 688 | Shift row | panel row (key) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |
| 689 | Reverse row | panel row (key) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |
| 690 | Station heading | panel row (heading) | PRESENT | a Table row of the plugin's panel page |
| 691 | Station settings row | panel row (key) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |
| 692 | Tune-to row | panel row | PRESENT | a Table row of the plugin's panel page |
| 693 | Tone rows | panel rows | PRESENT | a Table row of the plugin's panel page |
| 694 | Polarity row | panel row | PRESENT | a Table row of the plugin's panel page |
| 695 | Presets 1-4 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 696 | Presets 5-7 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |

## Plugins: broadcast and text — FT8 Decoder (ft8-decoder 1.1.1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 697 | Callsign | setting (text, 15) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 698 | Locator | setting (text, 6) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 699 | Report | setting (bool) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 700 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 701 | Reporting row | status row | PRESENT | a Table row of the plugin's panel page |
| 702 | Locator row | status row | PRESENT | a Table row of the plugin's panel page |
| 703 | Status row | status row | PRESENT | a Table row of the plugin's panel page |
| 704 | Last send | status row | PRESENT | a Table row of the plugin's panel page |
| 705 | Spots sent | status row | PRESENT | a Table row of the plugin's panel page |
| 706 | Waiting to send | status row | PRESENT | a Table row of the plugin's panel page |
| 707 | Decode lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 708 | Start message | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 709 | Diagnostic messages | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 710 | Presets (13) | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |

## Plugins: broadcast and text — DAB / DAB+ Radio (dab 0.5.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 711 | Service number | setting (number) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 712 | Station name | setting (text) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 713 | Decoder path | setting (path) + BROWSE FILES | PRESENT | a SettingField (PATH) in PluginSettingsForm, typed as a text box; the BROWSE FILES dialog is deferred (it needs the native handle) |
| 714 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 715 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 716 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 717 | Receiver row | status row | PRESENT | a Table row of the plugin's panel page |
| 718 | Sync row | status row | PRESENT | a Table row of the plugin's panel page |
| 719 | Transmission row | status row | PRESENT | a Table row of the plugin's panel page |
| 720 | Block row | status row | PRESENT | a Table row of the plugin's panel page |
| 721 | Ensemble row | status row | PRESENT | a Table row of the plugin's panel page |
| 722 | Audio row | status row | PRESENT | a Table row of the plugin's panel page |
| 723 | Decoder rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 724 | Text row | status row | PRESENT | a Table row of the plugin's panel page |
| 725 | DL Plus rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 726 | Slideshow row | status row | PRESENT | a Table row of the plugin's panel page |
| 727 | Service list | panel rows (cols) | PRESENT | a Table row of the plugin's panel page |
| 728 | Footer rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 729 | Slideshow picture | picture | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 730 | Presets (16) | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 731 | Decoder messages | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: broadcast and text — APRS / AX.25 Packet Decoder (aprs-decoder 1.0.2)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 732 | Frame line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 733 | Station track | map track | PRESENT | a target on the map page (MapPane: a station icon, labelled, with a row in the target list) |
| 734 | Station trail | map path | PRESENT | a Trails line on the map page (MapPane), faded with its target |
| 735 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 736 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |

## Plugins: broadcast and text — EAS / SAME Alert Decoder (eas-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 737 | Alert block | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 738 | Bad header | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 739 | End of message | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 740 | Attention tone | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 741 | Presets (7) | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |

## Plugins: broadcast and text — IBOC Radio (nrsc5 library) (nrsc5-radio-dynamic 0.1.2, EXPERIMENTAL, Windows only)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 742 | Programme | setting (kind unclear, likely number; key ) | PRESENT | a SettingField in PluginSettingsForm (the inventory could not confirm its kind) |
| 743 | Picture page | setting (text; key ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 744 | Auto gain | setting (bool; key ) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 745 | Library path | setting (path; key ) + BROWSE FILES | PRESENT | a SettingField (PATH) in PluginSettingsForm, typed as a text box; the BROWSE FILES dialog is deferred (it needs the native handle) |
| 746 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 747 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 748 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 749 | Dashboard programme rows | picture hit-target | PRESENT | a click on an interactive picture sends FOXAPP_OP_IMAGE_PRESS in picture pixels (PicturePane) |
| 750 | Programme legend | picture text | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 751 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 752 | Library rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 753 | Signal rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 754 | Station rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 755 | Now playing rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 756 | Services / objects rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 757 | Spectrum marks | mark | PRESENT | the main screen draws MARKERS on the spectrum and the waterfall (phase 1) |
| 758 | Auto-gain notices | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 759 | Decoder messages | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: broadcast and text — Nearby Signal Catch (nearby-signal-catch 0.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 760 | Mode key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 761 | Bands key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 762 | Level + | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 763 | Level - | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 764 | Margin + | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 765 | Margin - | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 766 | Ignore | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 767 | Clear ignores | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 768 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 769 | Disclaimer row | status row | PRESENT | a Table row of the plugin's panel page |
| 770 | Mode row | status row | PRESENT | a Table row of the plugin's panel page |
| 771 | Timing row | status row | PRESENT | a Table row of the plugin's panel page |
| 772 | Receiver state row | status row | PRESENT | a Table row of the plugin's panel page |
| 773 | Now row | status row | PRESENT | a Table row of the plugin's panel page |
| 774 | Stranded row | status row | PRESENT | a Table row of the plugin's panel page |
| 775 | Sweep row | status row | PRESENT | a Table row of the plugin's panel page |
| 776 | Ignore summary | status row | PRESENT | a Table row of the plugin's panel page |
| 777 | Hints | status rows | PRESENT | a Table row of the plugin's panel page |
| 778 | Hit log | rows (separator, then newest first) | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 779 | Spectrum mark | mark | PRESENT | the main screen draws MARKERS on the spectrum and the waterfall (phase 1) |
| 780 | Alert message | message (WARN, also plate notice) | PRESENT | the Decoder output page (text out, the plugin name dim before the line, WARN amber) |
| 781 | End message | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 782 | Key feedback | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: broadcast and text — DMR Monitor (data only) (dmr-monitor 0.1.1, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 783 | Hello line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 784 | Group call line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 785 | Private call line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 786 | Preset | preset key | PRESENT | a key of the PresetBar on the rail entry and the Decoder output page |

## Plugins: broadcast and text — TETRA Monitor (data only, no voice) (tetra-decoder 0.1.2)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 787 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 788 | Banner | status row (WARN, wrapped) | PRESENT | a Table row of the plugin's panel page (WARN tone) |
| 789 | CELL heading | heading | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 790 | Status | status row | PRESENT | a Table row of the plugin's panel page |
| 791 | Cell fields | status rows | PRESENT | a Table row of the plugin's panel page |
| 792 | SYSTEM INFO | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 793 | TRAFFIC | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 794 | LEGAL | heading + wrapped text | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 795 | Text lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 796 | Presets (3) | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |

## Plugins: broadcast and text — Radar Sweep (radar-sweep 1.0.2) - NOT in index.json

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 797 | Plate key | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 798 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 799 | Radar table columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 800 | Summary rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 801 | Receiver rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 802 | Radar track | map track | PRESENT | a target on the map page (MapPane: a station icon, labelled, with a row in the target list) |
| 803 | Target track | map track | PRESENT | a target on the map page (MapPane: an aircraft icon, labelled, with a row in the target list) |
| 804 | Text | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: broadcast and text — Digital Radio Mondiale (id 0.1.0) - NOT published

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 805 | Panel columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 806 | Sync row | status row | PRESENT | a Table row of the plugin's panel page |
| 807 | FAC/SDC row | status row | PRESENT | a Table row of the plugin's panel page |
| 808 | Mode E / Hierarchical | status rows | PRESENT | a Table row of the plugin's panel page |
| 809 | Multiplex row | status row | PRESENT | a Table row of the plugin's panel page |
| 810 | Audio row | status row | PRESENT | a Table row of the plugin's panel page |
| 811 | Service list | rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 812 | Footer | status rows | PRESENT | a Table row of the plugin's panel page |
| 813 | Presets (16) | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |

## Plugins: paging, meters and tools — Two-Tone Paging Decoder (twotone-decoder 1.0.1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 814 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 815 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 816 | Instrument window | instrument face (TONE_ALERT) | RELOCATED | the instrument page (a docked tab or a torn window) with the ToneAlertFace |
| 817 | Tone A / B bars | readout | PRESENT | a slot of ToneAlertFace |
| 818 | Code plate | readout | PRESENT | a slot of ToneAlertFace |
| 819 | Format plate | readout | PRESENT | a slot of ToneAlertFace |
| 820 | Verdict word | readout | PRESENT | a slot of ToneAlertFace |
| 821 | Alert lamp | instrument flag (ALERT) | PRESENT | a lamp of ToneAlertFace |
| 822 | Call log column 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 823 | Call log column 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 824 | Call log column 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 825 | Call log column 4 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 826 | Call log capacity | row feed | PRESENT | the memory table beneath the face (Table, virtual body); the depth is the plugin's |
| 827 | Decoder text line (pair) | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 828 | Decoder text line (single) | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — POCSAG Pager Decoder (pocsag-decoder 1.0.5)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 829 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 830 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 831 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 832 | Preset 4 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 833 | Preset 5 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 834 | Preset 6 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 835 | Numeric messages | setting (bool, key ) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 836 | Use function codes | setting (bool, key ) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 837 | Show frame details | setting (bool, key ) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 838 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 839 | Empty-history row | status row | PRESENT | a Table row of the plugin's panel page |
| 840 | Page rows | panel row (GOOD) | PRESENT | a Table row of the plugin's panel page |
| 841 | Frame details heading | panel heading | PRESENT | a Table row of the plugin's panel page |
| 842 | Frame rows | panel rows | PRESENT | a Table row of the plugin's panel page |
| 843 | Raw codewords heading + rows | panel heading/rows | PRESENT | a Table row of the plugin's panel page |
| 844 | Truncation row | panel row (WARN) | PRESENT | a Table row of the plugin's panel page |
| 845 | Reception status heading | panel heading | PRESENT | a Table row of the plugin's panel page |
| 846 | Status rows | panel rows | PRESENT | a Table row of the plugin's panel page |
| 847 | History-missed row | panel row (WARN) | PRESENT | a Table row of the plugin's panel page |
| 848 | Overflow row | panel row (WARN) | PRESENT | a Table row of the plugin's panel page |
| 849 | Decoder text line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — FLEX Pager Decoder (flex-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 850 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 851 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 852 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 853 | Preset 4 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 854 | Pager window | instrument face (PAGER) | RELOCATED | the instrument page (a docked tab or a torn window) with the PagerFace |
| 855 | Message display | readout | PRESENT | a slot of PagerFace |
| 856 | Capcode | readout | PRESENT | a slot of PagerFace |
| 857 | Time | readout | PRESENT | a slot of PagerFace |
| 858 | Kind | readout | PRESENT | a slot of PagerFace |
| 859 | Unread count | readout | PRESENT | a slot of PagerFace |
| 860 | Alert lamp | instrument flag (ALERT) | PRESENT | a lamp of PagerFace |
| 861 | Sync lamp | instrument flag (LOCK) | PRESENT | a lamp of PagerFace |
| 862 | Low battery lamp | instrument flag | PRESENT | a lamp of PagerFace; the plugin never sets it |
| 863 | Memory column 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 864 | Memory column 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 865 | Memory column 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 866 | Memory depth | row feed | PRESENT | the memory table beneath the face (Table, virtual body); the depth is the plugin's |
| 867 | Decoder text line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — ERT Utility Meter Decoder (ert-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 868 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 869 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 870 | Meter window | instrument face (METER) | RELOCATED | the instrument page (a docked tab or a torn window) with the MeterFace |
| 871 | Meter id | readout | PRESENT | a slot of MeterFace |
| 872 | Commodity lamps | readout | PRESENT | the lamps of MeterFace |
| 873 | Reading | readout | PRESENT | a slot of MeterFace |
| 874 | Tamper counters | readout | PRESENT | a slot of MeterFace |
| 875 | Tamper alert | instrument flag (ALERT) | PRESENT | a lamp of MeterFace |
| 876 | Roster column 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 877 | Roster column 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 878 | Roster column 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 879 | Roster column 4 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 880 | Roster capacity | row feed | PRESENT | the memory table beneath the face (Table, virtual body); the depth is the plugin's |
| 881 | Decoder text line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 882 | Status line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — Wireless M-Bus Meter Decoder (wmbus-decoder 0.1.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 883 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 884 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 885 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 886 | AES keys | setting (text, key , max 4000 chars) | PRESENT | a SettingField (TEXT) in PluginSettingsForm; a long value goes up in full (submit_text) |
| 887 | Settings loaded log | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 888 | Meter window | instrument face (METER) | RELOCATED | the instrument page (a docked tab or a torn window) with the MeterFace |
| 889 | Meter id | readout | PRESENT | a slot of MeterFace |
| 890 | Commodity | readout | PRESENT | a slot of MeterFace |
| 891 | Reading + unit | readout | PRESENT | a slot of MeterFace |
| 892 | Alarm lamp | instrument flag (ALERT) | PRESENT | a lamp of MeterFace |
| 893 | Low-battery lamp | instrument flag (LOW_BATT) | PRESENT | a lamp of MeterFace |
| 894 | Roster col 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 895 | Roster col 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 896 | Roster col 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 897 | Roster col 4 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 898 | Roster col 5 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 899 | Roster col 6 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 900 | Roster col 7 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 901 | Roster capacity | row feed | PRESENT | the memory table beneath the face (Table, virtual body); the depth is the plugin's |
| 902 | Decoder text line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 903 | Status line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — Example RMS Reporter (example-decoder 2.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 904 | Decoder text line | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 905 | Rail row / chip | rail row (host) | PRESENT | a Section row on the DECODE bank (open / close and the run state) |

## Plugins: paging, meters and tools — Decoder Workbench (workbench 0.2.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 906 | Presets (one per band) | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 907 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 908 | Column 1 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 909 | Column 2 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 910 | Column 3 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 911 | Column 4 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 912 | Column 5 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 913 | Warning row | status row (WARN) | PRESENT | a Table row of the plugin's panel page (WARN tone) |
| 914 | Panel refresh | behaviour | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |
| 915 | Routing announcement | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 916 | Decoder text lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — Demod Analyzer (demod-analyzer 0.1.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 917 | Dashboard picture | image window | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 918 | ANALYSE NOW | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 919 | HOLD / RUN LIVE | plate key (toggles) | PRESENT | a key of PlateKeys on the module page (the plugin's own wording; its label follows the state) |
| 920 | PRINT BITS | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 921 | EXPORT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 922 | Source | panel row | PRESENT | a Table row of the plugin's panel page |
| 923 | Status | panel row | PRESENT | a Table row of the plugin's panel page |
| 924 | Modulation | panel row | PRESENT | a Table row of the plugin's panel page |
| 925 | Confidence | panel row | PRESENT | a Table row of the plugin's panel page |
| 926 | Measurement rows | panel rows | PRESENT | a Table row of the plugin's panel page |
| 927 | Result row | panel row (WARN) | PRESENT | a Table row of the plugin's panel page |
| 928 | I/Q file | setting (text, key ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 929 | File format | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 930 | File sample rate | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 931 | Skip first samples | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 932 | Modulation | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 933 | Symbol rate | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 934 | Carrier offset | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 935 | Extra rotation | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 936 | Matched filter | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 937 | Roll-off | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 938 | Filter span | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 939 | Sampling offset | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 940 | Symbol range | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 941 | Sync word | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 942 | Remove DC offset | setting (bool, ) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 943 | Live capture | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 944 | Spectrum point mark | mark | PRESENT | the main screen draws MARKERS on the spectrum and the waterfall (phase 1) |
| 945 | Occupied-band mark | mark (span, spectrum only) | PRESENT | the main screen draws MARKERS on the spectrum and the waterfall (phase 1) |
| 946 | Decoder text lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: paging, meters and tools — Packet TNC (KISS) (packet-tnc 0.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 947 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 948 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 949 | KISS TCP port | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 950 | Bind address | setting (text, , max 45) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 951 | Channel busy above (dB) | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 952 | TX amplitude (0.05-1) | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 953 | TX gives up after (s) | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 954 | My callsign (panel only) | setting (text, , max 9) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 955 | RESTART SERVER | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 956 | DROP TX QUEUE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 957 | GRANT TRANSMIT | host plate key | PRESENT | GrantKeys on the module page: GRANT / REVOKE TRANSMIT, shown once the plugin has asked |
| 958 | Transmit audio | transmit | PRESENT | the GRANT TRANSMIT key and the transmit plate sentence on the module page; the audio path is the engine's |
| 959 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 960 | KISS server block | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 961 | Receive block | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 962 | Transmit block | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 963 | Host transmit state block | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 964 | KISS parameters block | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 965 | Last frames block | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 966 | Decoder text lines | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 967 | Network | TCP listener | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |

## Plugins: paging, meters and tools — Radio Email (Winlink-compatible) (radio-email 0.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 968 | Callsign | setting (text, , max 16) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 969 | Winlink password (write-only) | setting (text, , max 64) | PRESENT | a SettingField (TEXT) in PluginSettingsForm; the plugin declares TEXT, so the box is not masked (the PASSWORD kind is the host's) |
| 970 | Locator | setting (text, , max 6) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 971 | To | setting (text, , max 255) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 972 | Cc | setting (text, ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 973 | Subject | setting (text, , max 128) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 974 | Message text | setting (text, , max 4095) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 975 | Attachment | setting (path, , max 520) | PRESENT | a SettingField (PATH) in PluginSettingsForm, typed as a text box; the BROWSE FILES dialog is deferred (it needs the native handle) |
| 976 | Gateway host | setting (text, , max 100) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 977 | Gateway port | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 978 | Auto-check, minutes (0 = off) | setting (number, ) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 979 | CONNECT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 980 | QUEUE MESSAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 981 | DELETE DRAFT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 982 | NEXT MESSAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 983 | PREVIOUS MESSAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 984 | MARK READ | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 985 | NEXT PAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 986 | PREVIOUS PAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 987 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 988 | Status heading | panel heading | PRESENT | a Table row of the plugin's panel page |
| 989 | Status rows | status rows | PRESENT | a Table row of the plugin's panel page |
| 990 | Mailbox list rows | panel rows (5 cells) | PRESENT | a Table row of the plugin's panel page |
| 991 | Message view | heading + rows | PRESENT | Table rows of the plugin's panel page (heading rows and cells) |
| 992 | Decoder output session log | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 993 | Network | outbound TCP | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |

## Plugins: paging, meters and tools — Bonus: Host API tour (hostapi-example 1.0.0; src/hostapi_example; not in index.json)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 994 | Mark here | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 995 | Clear marks | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 996 | Up 25 kHz | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 997 | AM 10 kHz | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 998 | Gain step | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 999 | Mute / Unmute | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1000 | Stop / Start | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1001 | Next rate | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1002 | Receiver panel | panel | PRESENT | a Table row of the plugin's panel page |
| 1003 | Soft limiter | audio processor | PRESENT | the plugin's own behaviour: the kit cannot see it, there is nothing to draw |
| 1004 | Level meter text | message list | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — Satellites (satellites 1.1.2)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1005 | Satellite markers | map track | RELOCATED | a target on the generic map page over the coastline (equirectangular, no tiles); the satellites deck is deferred |
| 1006 | Next-orbit ground track | map track (path) | RELOCATED | drawn as a Trails line on the generic map page over the coastline; the satellites deck is deferred |
| 1007 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 1008 | Column 1 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1009 | Column 2 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1010 | Column 3 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1011 | Column 4 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1012 | Column 5 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1013 | Heading row | panel row | PRESENT | a Table row of the plugin's panel page |
| 1014 | Satellites status row | status row | PRESENT | a Table row of the plugin's panel page |
| 1015 | How-to row | status row | PRESENT | a Table row of the plugin's panel page |
| 1016 | Mirror row | status row | PRESENT | a Table row of the plugin's panel page |
| 1017 | Unmatched-track row | status row | PRESENT | a Table row of the plugin's panel page |
| 1018 | Carried-set row | status row | PRESENT | a Table row of the plugin's panel page |
| 1019 | Lookup-trim row | status row | PRESENT | a Table row of the plugin's panel page |
| 1020 | No-observer row | status row | PRESENT | a Table row of the plugin's panel page |
| 1021 | Passes heading | panel row | PRESENT | a Table row of the plugin's panel page |
| 1022 | Computing row | status row | PRESENT | a Table row of the plugin's panel page |
| 1023 | Pass row (LEO) | panel row | PRESENT | a Table row of the plugin's panel page |
| 1024 | Look-angle row (geostationary) | panel row | PRESENT | a Table row of the plugin's panel page |
| 1025 | Doppler heading | panel row | PRESENT | a Table row of the plugin's panel page |
| 1026 | Doppler row | readout | PRESENT | a Table row on the plugin's panel page |
| 1027 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1028 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1029 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1030 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1031 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1032 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1033 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1034 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |

## Plugins: satellites, weather and maps — NOAA Weather Pictures (APT) (apt-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1035 | APT picture | picture | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 1036 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1037 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1038 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1039 | Start line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1040 | Lock line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1041 | Progress line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1042 | Complete line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — Meteor-M LRPT Weather Pictures (lrpt-decoder 0.1.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1043 | LRPT picture | picture | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 1044 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1045 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1046 | Banner lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1047 | Refused line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1048 | Carrier / lock lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1049 | Quality / mapping lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1050 | Packet lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — GOES Weather Satellites (HRIT / LRIT) (goes 0.2.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1051 | GOES picture | picture | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 1052 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 1053 | Column 1 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1054 | Column 2 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1055 | Column 3 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1056 | Idle row | status row | PRESENT | a Table row of the plugin's panel page |
| 1057 | Rate row | status row | PRESENT | a Table row of the plugin's panel page |
| 1058 | Heading | panel row | PRESENT | a Table row of the plugin's panel page |
| 1059 | Link row | status row | PRESENT | a Table row of the plugin's panel page |
| 1060 | Level row | status row | PRESENT | a Table row of the plugin's panel page |
| 1061 | Eb/N0 row | status row | PRESENT | a Table row of the plugin's panel page |
| 1062 | Channel BER row | status row | PRESENT | a Table row of the plugin's panel page |
| 1063 | Heading | panel row | PRESENT | a Table row of the plugin's panel page |
| 1064 | Sync row | status row | PRESENT | a Table row of the plugin's panel page |
| 1065 | CADUs row | status row | PRESENT | a Table row of the plugin's panel page |
| 1066 | RS row | status row | PRESENT | a Table row of the plugin's panel page |
| 1067 | VCDUs lost row | status row | PRESENT | a Table row of the plugin's panel page |
| 1068 | Channels row | status row | PRESENT | a Table row of the plugin's panel page |
| 1069 | Heading | panel row | PRESENT | a Table row of the plugin's panel page |
| 1070 | Packets row | status row | PRESENT | a Table row of the plugin's panel page |
| 1071 | Files row | status row | PRESENT | a Table row of the plugin's panel page |
| 1072 | Segments row | status row | PRESENT | a Table row of the plugin's panel page |
| 1073 | Written row | status row | PRESENT | a Table row of the plugin's panel page |
| 1074 | Rice-refused row | status row | PRESENT | a Table row of the plugin's panel page |
| 1075 | Already-decompressed row | status row | PRESENT | a Table row of the plugin's panel page |
| 1076 | EMWIN row | status row | PRESENT | a Table row of the plugin's panel page |
| 1077 | Image row | status row | PRESENT | a Table row of the plugin's panel page |
| 1078 | Channel row | status row | PRESENT | a Table row of the plugin's panel page |
| 1079 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1080 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1081 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1082 | Message lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1083 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1084 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |

## Plugins: satellites, weather and maps — WEFAX Weather Fax Decoder (wefax-decoder 1.1.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1085 | Chart picture | picture | PRESENT | the picture page (PicturePane): fitted, Save as BMP, the status line |
| 1086 | Click on chart | image input | PRESENT | a click on an interactive picture sends FOXAPP_OP_IMAGE_PRESS in picture pixels (PicturePane) |
| 1087 | Recorder window | instrument face (FAX) | RELOCATED | the instrument page (a docked tab or a torn window) with the FaxFace |
| 1088 | Face value 1 | readout | PRESENT | a slot of FaxFace |
| 1089 | Face value 2 | readout | PRESENT | a slot of FaxFace |
| 1090 | Face value 3 | readout | PRESENT | a slot of FaxFace |
| 1091 | Face value 4 | readout | PRESENT | a slot of FaxFace |
| 1092 | Phase lamps | readout | PRESENT | the lamps of FaxFace |
| 1093 | LOCK flag | readout | PRESENT | the lamps of FaxFace |
| 1094 | Memory column 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 1095 | Memory column 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 1096 | Log events | message list | PRESENT | the recorder's memory table rows (Table), newest first |
| 1097 | START NOW | panel row (pressable key) + plate key | PRESENT | a pressable Table row (hand cursor) and a key of PlateKeys on the module page |
| 1098 | STOP NOW | panel row (pressable key) + plate key | PRESENT | a pressable Table row (hand cursor) and a key of PlateKeys on the module page |
| 1099 | SLANT -10 ppm | panel row (key) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |
| 1100 | SLANT +10 ppm | panel row (key) | PRESENT | a pressable Table row (hand cursor); a press sends FOXAPI_OP_PLUGIN_COMMAND |
| 1101 | SET LEFT EDGE | panel row (key) + plate key | PRESENT | a pressable Table row (hand cursor) and a key of PlateKeys on the module page |
| 1102 | Plate keys (same five, uppercase wording differs) | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1103 | Slant/edge figures | readout | PRESENT | a slot of FaxFace |
| 1104 | Setting: slant | settings field (NUMBER) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1105 | Setting: hand-start rate | settings field (NUMBER) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1106 | Setting: hand-start IOC | settings field (NUMBER) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1107 | Station guide window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 1108 | Guide columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1109 | Guide rows | panel row | PRESENT | a Table row of the plugin's panel page |
| 1110 | Presets | preset key x10 | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1111 | Text lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — Radiosondes (radiosonde-decoder 0.1.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1112 | Balloon marker | map track | PRESENT | a target on the map page (MapPane: an icon by kind, labelled, with a row in the target list); the lighter-than-air silhouette is deferred with the category silhouettes |
| 1113 | Balloon trail | map track (path) | PRESENT | a Trails line on the map page (MapPane), faded with its target |
| 1114 | Frame line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1115 | No-fix line | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1116 | Carrier lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1117 | Rate warning | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1118 | Preset 1 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1119 | Preset 2 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1120 | Preset 3 | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |

## Plugins: satellites, weather and maps — 433 MHz Weather Sensor Decoder (weather-decoder 1.0.0)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1121 | Console window | instrument face (WEATHER_CONSOLE) | RELOCATED | the instrument page (a docked tab or a torn window) with the WeatherFace |
| 1122 | Channel 1-3 temperature | readout | PRESENT | a slot of WeatherFace |
| 1123 | Channel 1-3 humidity | readout | PRESENT | a slot of WeatherFace |
| 1124 | Channel 1-3 sensor model | readout | PRESENT | a slot of WeatherFace |
| 1125 | Low-battery flag | readout | PRESENT | the lamps of WeatherFace |
| 1126 | Memory column 1 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 1127 | Memory column 2 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 1128 | Memory column 3 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 1129 | Memory column 4 | column | PRESENT | a column of the memory table beneath the face (Table) |
| 1130 | Preset | preset key | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1131 | Warning lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — Map Imagery (OpenStreetMap) (openmaptiles-basemap 1.1.2)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1132 | Map tiles | map layer | PRESENT | MapPane draws the tiles from the basemap plugin under Mercator while it is fitted |
| 1133 | Attribution | readout | PRESENT | a Table row on the plugin's panel page |
| 1134 | Status lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |
| 1135 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1136 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1137 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |

## Plugins: satellites, weather and maps — Survey Engine (survey-engine 0.1.1)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1138 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 1139 | Column 1 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1140 | Column 2 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1141 | Column 3 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1142 | Column 4 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1143 | Column 5 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1144 | Column 6 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1145 | Column 7 | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1146 | Summary row | status row | PRESENT | a Table row of the plugin's panel page |
| 1147 | Cannot-sweep row | status row | PRESENT | a Table row of the plugin's panel page |
| 1148 | Config key / | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1149 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1150 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1151 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1152 | Config key | config file key | PRESENT | a config file / env var: not a window control (the inventory lists it for completeness) |
| 1153 | Text lines | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — Propagation Tools (propagation-tools 0.1.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1154 | Picture window | picture | RELOCATED | the picture page (PicturePane): a docked tab or a torn window |
| 1155 | Page 1 WORLD MAP | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1156 | Page 2 BEACONS | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1157 | Page 3 METEORS | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1158 | Page 4 SOLAR | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1159 | Page 5 VOACAP | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1160 | NEXT PAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1161 | PREV PAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1162 | REFRESH | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1163 | NEXT HOUR | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1164 | PREV HOUR | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1165 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 1166 | Columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1167 | Status block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1168 | Solar block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1169 | Beacons block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1170 | Meteors block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1171 | Path block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1172 | VOACAP engines block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1173 | Credits block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1174 | Setting: locator | settings field (TEXT, max 8) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1175 | Setting: target | settings field (TEXT, max 8) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1176 | Setting: heat map | settings field (BOOL, default on) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1177 | Setting: network | settings field (BOOL, default on) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1178 | Setting: engine | settings field (TEXT, max 12, default ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1179 | Setting: VOACAP path | settings field (PATH, max 512) | PRESENT | a SettingField (PATH) in PluginSettingsForm, typed as a text box; the BROWSE FILES dialog is deferred (it needs the native handle) |
| 1180 | Setting: SSN | settings field (NUMBER, default 0, range 0-400) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1181 | Setting: power | settings field (NUMBER, default 100, range 0.1-100000) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1182 | Setting: antenna | settings field (TEXT, max 24, default ) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1183 | Setting: gain | settings field (NUMBER, default 0, range -10..30) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1184 | Setting: noise | settings field (NUMBER, default 145, range 100-180) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1185 | Setting: required SNR | settings field (NUMBER, default 24, range 0-80) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1186 | Setting: map threshold | settings field (NUMBER, default 50, range 1-100) | PRESENT | a SettingField (NUMBER) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1187 | Beacon presets | preset key x5 | PRESENT | a key of the PresetBar on the plugin's page and the rail entry |
| 1188 | Warning log | message | PRESENT | the Decoder output page (text out, the plugin name dim before the line) |

## Plugins: satellites, weather and maps — Solar System View (solar-system-view 0.1.0, EXPERIMENTAL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1189 | Picture window | picture | RELOCATED | the picture page (PicturePane): a docked tab or a torn window |
| 1190 | SYSTEM page | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1191 | SUN page | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1192 | CORONA page | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1193 | AURORA page | picture page | PRESENT | inside the picture the plugin rasterises; PicturePane shows it fitted |
| 1194 | ROTATE LEFT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1195 | ROTATE RIGHT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1196 | TILT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1197 | ZOOM IN | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1198 | ZOOM OUT | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1199 | NEXT PAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1200 | PREVIOUS PAGE | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1201 | NEXT CHANNEL | plate key | PRESENT | a key of PlateKeys on the module page (the plugin's own wording) |
| 1202 | Setting: locator | settings field (TEXT, max 6) | PRESENT | a SettingField (TEXT) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1203 | Setting: log orbits | settings field (BOOL, default off) | PRESENT | a SettingField (BOOL) in PluginSettingsForm, on the plugin's page and the rail entry |
| 1204 | Panel window | panel | RELOCATED | the panel page: a docked tab or a torn window, not an OS window |
| 1205 | Columns | column | PRESENT | a Table column of the plugin's panel page (the plugin's headings) |
| 1206 | Solar wind block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1207 | CME block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1208 | Aurora block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1209 | Planets block | panel rows | PRESENT | a Table row of the plugin's panel page |
| 1210 | Feeds block | panel rows | PRESENT | a Table row of the plugin's panel page |

# Phase 3: the SIGNAL bank (docs 6.39) - `inventory/signal-bank.txt` walked row by row

Every row of `inventory/signal-bank.txt` (FoxSDR 0.99.104; the file calls itself "about 156 controls" and holds 176 rows) against the SIGNAL bank of the Slint window: the bodies of `src/slint/ui/signal_bank.slint` (`SourceBody` with its eight variants, `FrequencyCorrectionBody`, `RadioExtras`, `AudioFiltersBody`, `SinksBody`, `TransmitRowState`, `RecorderBody`, `RadioSetupSheet`, `ImportDialog`), their commands (`src/slint/signal_logic.hpp`) and the places `src/slint/ui/main.slint` puts them (bank 0, rows 0-7: Patch, Source, Radio, Audio filters, Sinks, Transmit, Recorder, Frequency correction). The status words are phase 1's: **PRESENT** = the family draws it and its callback has a command builder or a local effect (the note names the component), **RELOCATED** = it lives elsewhere in the Slint window (the receiver strip, the tab row, the Section row itself, a sheet of `main.slint`), **DEFERRED** = not drawn, or drawn wrongly, with the reason. A control whose status could not be established from the files is DEFERRED with "not found in signal_bank.slint". The import dialog also draws an 'Import as' dropdown that the inventory does not list. The host's publisher (`signal_publish`) was not in the tree when this part was ticked: PRESENT here means drawn and given a command in `signal_logic`, not seen working in the window.

## SIGNAL bank — Patch (rail row, SIGNAL PATH bank)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1211 | Patch switch row | key (latched toggle) | RELOCATED | the PATCH tab of the tab row flips the view (row 69); the rail row itself is the Section row 'Patch' of bank 0, a fold with no body |
| 1212 | Patch chip | readout | PRESENT | the Section row's chip: PATCH RUNNING (amber) or RECEIVER VIEW, from FOXAPI_RX_PATCH_RUNNING; the table carries no node count, so there is no 'n nodes' word |
| 1213 | Patch lamp | lamp/indicator | RELOCATED | the lit PATCH tab (on while the patch view shows); the Section row draws no dot |
| 1214 | Patch tooltip | tooltip | RELOCATED | the PATCH tab's tooltip carries the same sentence |

## SIGNAL bank — Source (rail section): header

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1215 | Source header | key (fold) | RELOCATED | the Section row 'Source' of bank 0 folds the body; its chip is the source name (RTL-SDR / OPEN / GENERATOR) and its dot is off in sectionsFor today (SourceModel.chip and the lamp fields are host extras the row does not read yet) |

## SIGNAL bank — Source (rail section): faults, advice and hints

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1216 | Device stopped | readout | PRESENT | SourceBody: a 'Device stopped' card in the error tone with the whole fault sentence (stopped-line) |
| 1217 | Reconnect advice | readout | PRESENT | the second line of the same card (stopped-advice); signal_logic leaves it out when the fault already says to restart FoxSDR |
| 1218 | Busy line | readout | PRESENT | the first paragraph of the Advisories card, amber ('Opening X...' / 'Scanning for devices...'); Refresh is greyed with it |
| 1219 | Driver crashed scan | readout | PRESENT | a paragraph of the Advisories card, amber |
| 1220 | Network USRP hint | readout | PRESENT | a paragraph of the Advisories card, dim; only with UHD left out and the box unticked |
| 1221 | UHD images hint | readout | PRESENT | a paragraph of the Advisories card, amber |
| 1222 | Scan-deferred explanation | readout + tooltip | PRESENT | a paragraph of the Advisories card (both sentences); the tooltip twin is not drawn, the sentence is on screen as text |
| 1223 | Zadig / WinUSB warning (one per unbound USB radio) | readout | PRESENT | one paragraph per unbound radio in the Advisories card (zadigAdvice: RTL-SDR, RX888 two-step, AOR, generic); the Radio-setup page repeats them as blocks |
| 1224 | SDRplay advice | readout | PRESENT | an amber advice paragraph and a dim 'SDRplay API: detail' paragraph in the Advisories card; hidden when an RSP row exists |

## SIGNAL bank — Source (rail section): the source list and its keys

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1225 | Source dropdown | dropdown/combo | PRESENT | SourceBody DropKey -> FOXAPI_OP_SELECT_SOURCE by the fact id; closed text '(saved, not open)' or 'the patch has ...'; the lazy native read on opening (FOXAPP_OP_SCAN_DEVICES_ON_OPEN, opScanOnOpen) has no callback in the contract, so opening the list does not send it |
| 1226 | Source row (device) | list | PRESENT | a DropRow per SelectItem; hardware rows grey while a patch runs; the explaining tooltip (SelectItem tooltip, a host extra) is not drawn, DropRow has none |
| 1227 | Refresh | button | PRESENT | small Key -> FOXAPI_OP_SCAN_DEVICES; greyed while a scan or open is busy |
| 1228 | Look for network USRPs | checkbox | PRESENT | Tick with the tooltip -> FOXAPP_OP_SET_NETWORK_USRP_SCAN |
| 1229 | RADIO SETUP | button (small) | PRESENT | xs Key -> source-setup (local: shows the Radio-setup sheet and starts the probe) |
| 1230 | Where FoxSDR looked | tree node (fold) | RELOCATED | its lines (vendor installs, search paths, modules) are on the Radio-setup page as plain lines under the heading, not a fold under the Source row |
| 1231 | No radio hardware block | readout | PRESENT | one amber paragraph of the Advisories card: the headline, the SoapySDR-modules sentence, the PothosSDR / radioconda or modules-loaded sentence, the Zadig paragraph |
| 1232 | Saved-source warning | readout | PRESENT | a paragraph of the Advisories card (file / sound card / radio wording), with the combo reading '<name> (saved, not open)' |
| 1233 | Tune-mismatch note | readout | PRESENT | amber Note at the foot of SourceBody |
| 1234 | Source error line | readout | PRESENT | red Note at the foot of SourceBody |

## SIGNAL bank — Source (rail section): I/Q file, Pluto and rtl_tcp

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1235 | I/Q file path | text field | PRESENT | variant 1 card: LineInput, commits on Enter and on focus lost, the draft is the host's (local); since docs 6.63 Ctrl+V takes a copied FILE's path (rows 1971, 1972 and the Browse key, row 1969, give it the chooser the first build lacked) |
| 1236 | I/Q file Open | button | PRESENT | Open key -> iq-open -> planIqOpen: SELECT_SOURCE 'file:' + the box CLEANED (core::typedIqPath); an empty box is the file dialog (row 1970); the 'waiting for the disk' line (iq-note) shows after 1 s, and since docs 6.63 Browse and Open grey while the header is being read (iq-opening, as the classic's BeginDisabled) |
| 1237 | Pluto address | text field | PRESENT | variant 2 card: LineInput -> FOXAPP_OP_SET_PLUTO_URI; not greyed while an open is pending |
| 1238 | Pluto Open | button | PRESENT | Open key -> SELECT_SOURCE 'open-pluto:uri=' + the typed address |
| 1239 | Pluto help | readout | PRESENT | the card's dim Note (pluto-help) |
| 1240 | rtl_tcp address | text field | PRESENT | variant 3 card: LineInput -> FOXAPP_OP_SET_RTLTCP_ADDR; not greyed while an open is pending |
| 1241 | rtl_tcp Open | button | PRESENT | Open key -> SELECT_SOURCE 'open-rtltcp:rtltcp=' + the typed address |
| 1242 | rtl_tcp help | readout | PRESENT | the card's dim Note (rtltcp-help) |

## SIGNAL bank — Source (rail section): sound card

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1243 | Busy line | readout | PRESENT | variant 4 card: amber Note (card-busy) |
| 1244 | No inputs | readout | PRESENT | amber Note (card-none) |
| 1245 | Device | dropdown/combo | PRESENT | DropKey of the cards -> FOXAPP_OP_SOUND_CARD_PICK; nothing for the card already chosen |
| 1246 | Format | dropdown/combo | PRESENT | DropKey 'Real (mono)' / 'I/Q (stereo)' -> FOXAPP_OP_SOUND_CARD_FORM, field FORMAT |
| 1247 | Channel | dropdown/combo | PRESENT | DropKey Left / Right -> SOUND_CARD_FORM, field CHANNEL; drawn only with Real (mono) |
| 1248 | Swap I/Q | checkbox | PRESENT | Tick -> SOUND_CARD_FORM, field SWAP_IQ; drawn only with I/Q (stereo) |
| 1249 | Centre (MHz) | number field | PRESENT | LineInput (mono, right) -> two commands, SOUND_CARD_FORM (centre) then FOXAPP_OP_SOUNDCARD_IQ_CENTRE; drawn only with I/Q (stereo) |
| 1250 | Sample rate | dropdown/combo | PRESENT | DropKey of the card's rates (exclusive rows lettered '(exclusive)'; their row tooltip is not drawn) -> SOUND_CARD_FORM, field RATE |
| 1251 | Open | button | PRESENT | Open key -> SELECT_SOURCE 'soundcard:open'; greyed while no sound card is chosen (SourceModel.card-can-open, docs 6.61; test_slint_signal reads the greyed look and that it asks nothing) |
| 1252 | Receives line | readout | PRESENT | dim Note (card-receives) |
| 1253 | Missing-card note | readout | PRESENT | amber Note (card-warning) |

## SIGNAL bank — Source (rail section): the open radio

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1254 | Rate | dropdown/combo | PRESENT | DropKey (108 px) -> FOXAPI_OP_SET_SAMPLE_RATE; shows the row nearest the radio's rate |
| 1255 | Actual rate | readout | PRESENT | 'actual x MS/s' readout on the Rate row |
| 1256 | Antenna | dropdown/combo | PRESENT | DropKey -> FOXAPI_OP_SET_ANTENNA; drawn only with more than one port |
| 1257 | Antenna (single) | readout | PRESENT | readout 'Antenna: X' in place of the dropdown |
| 1258 | Auto gain | checkbox | PRESENT | Tick -> FOXAPI_OP_SET_DEVICE_AGC; the model's value is pushed back, so a refusal springs back; not drawn for an Airspy |
| 1259 | Auto gain unsupported | readout | PRESENT | dim Note (agc-note) |
| 1260 | Gain stage sliders | slider | PRESENT | a SliderRow per stage (the driver's name, readout in dB or steps) -> FOXAPI_OP_SET_GAIN; greyed with Auto gain; not drawn for an Airspy |
| 1261 | Bias tee | checkbox | PRESENT | Tick with the 4.5 V tooltip; off is sent at once, on asks the bias question first (a deliberate change: the ImGui box did not ask) |
| 1262 | FM notch | checkbox | PRESENT | the 'Radio extras' card (variant 6): an OptionRow Tick with its tooltip -> FOXAPI_OP_SET_DEVICE_OPTION; drawn only on a radio whose model has it |
| 1263 | DAB notch | checkbox | PRESENT | the 'Radio extras' card (variant 6): an OptionRow Tick with its tooltip -> FOXAPI_OP_SET_DEVICE_OPTION; drawn only on a radio whose model has it |
| 1264 | HDR mode | checkbox | PRESENT | the 'Radio extras' card (variant 6): an OptionRow Tick with its tooltip -> FOXAPI_OP_SET_DEVICE_OPTION; drawn only on a radio whose model has it |
| 1265 | ADC dither | checkbox | PRESENT | the 'Radio extras' card (variant 6): an OptionRow Tick with its tooltip -> FOXAPI_OP_SET_DEVICE_OPTION; drawn only on a radio whose model has it |
| 1266 | ADC randomiser | checkbox | PRESENT | the 'Radio extras' card (variant 6): an OptionRow Tick with its tooltip -> FOXAPI_OP_SET_DEVICE_OPTION; drawn only on a radio whose model has it |

## SIGNAL bank — Source (rail section): Airspy panel

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1267 | Decimation | dropdown/combo | PRESENT | variant 5 card: DropKey (None, 2, 4 ...) with the tooltip -> FOXAPP_OP_AIRSPY_DECIMATION |
| 1268 | Decimation readout | readout | PRESENT | dim Note ('radio at x MS/s, decimated by n'); only above 1 |
| 1269 | Gain mode label | readout | PRESENT | Caption 'Gain mode' above the three radios |
| 1270 | Sensitive | radio | PRESENT | RadioChoice with its tooltip -> FOXAPP_OP_AIRSPY_GAIN_MODE |
| 1271 | Linear | radio | PRESENT | RadioChoice with its tooltip -> FOXAPP_OP_AIRSPY_GAIN_MODE |
| 1272 | Free | radio | PRESENT | RadioChoice with its tooltip -> FOXAPP_OP_AIRSPY_GAIN_MODE |
| 1273 | Gain (Sensitive/Linear) | slider | PRESENT | SliderRow (no figure) -> FOXAPI_OP_SET_GAIN on the first stage, whole steps |
| 1274 | LNA AGC | checkbox | PRESENT | Tick -> FOXAPP_OP_AIRSPY_AGC; drawn in Free mode only |
| 1275 | Mixer AGC | checkbox | PRESENT | Tick -> FOXAPP_OP_AIRSPY_AGC; drawn in Free mode only |
| 1276 | LNA / MIXER / VGA sliders | slider | PRESENT | three SliderRows (LNA, MIXER, VGA) -> FOXAPI_OP_SET_GAIN by stage name; LNA and MIXER grey with their AGC; Free mode only |

## SIGNAL bank — Source (rail section): converter

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1277 | Converter heading | readout | PRESENT | the Converter card's tag |
| 1278 | Converter mode | dropdown/combo | PRESENT | DropKey Off / Up-converter / Down-converter with the tooltip -> FOXAPP_OP_SET_CONVERTER; Up is greyed for a real (mono) sound card |
| 1279 | Alias / unusable notes | readout | PRESENT | amber Note (converter-alias) |
| 1280 | Local oscillator | text field | PRESENT | LineInput (mono, right) with the tooltip, red border on a parse error; Enter -> SET_CONVERTER with the parsed LO; drawn only with a mode other than Off |
| 1281 | LO quick-picks (3) | button (small) | DEFERRED | drawn but out of step with its command: the keys read 100 / 125 / 10 MHz and send index 0 / 1 / 2, which signal_logic maps to 125 / 100 / 2 MHz (kConverterQuickLoHz, the ImGui window's kQuickLoHz), so '100 MHz' sets 125 MHz and '10 MHz' sets 2 MHz; relabel the keys 125 / 100 / 2 MHz in signal_bank.slint before this row counts |
| 1282 | Inverts the spectrum | checkbox | PRESENT | Tick with the tooltip -> FOXAPP_OP_SET_CONVERTER (inverted); drawn only with a mode other than Off |
| 1283 | LO parse error | readout | PRESENT | the LO field's red border and an amber Note (converter-error) |
| 1284 | On-air line | readout | PRESENT | dim Note (converter-on-air) |

## SIGNAL bank — Source (rail section): frequency correction

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1285 | Frequency correction heading | readout | RELOCATED | the Section row 7 'Frequency correction' of the SIGNAL bank (the Slint SYSTEM bank has no such section); SourceBody keeps a Caption for it only when its ppm-block is on, and main.slint turns that off |
| 1286 | PPM frequency correction | checkbox | PRESENT | FrequencyCorrectionBody Tick with the tooltip -> FOXAPP_OP_SET_PPM (the switch keeps the stored value); not greyed while an open is pending |
| 1287 | Not-applicable note | readout | PRESENT | dim Note (ppm-note) when the source has no crystal |
| 1288 | Whose value | readout | PRESENT | Note 'For <model>, serial <s>' (ppm-radio-line) |
| 1289 | ppm value | number field (with step arrows) | PRESENT | mono LineInput with a 'ppm' unit and two step arrows (+-0.1, committed as text); Enter or focus lost -> SET_PPM with the sanitised value, nothing when unchanged |
| 1290 | How applied | readout | PRESENT | dim Note (ppm-note: the method) |
| 1291 | Refusal | readout | PRESENT | amber Note (ppm-refusal) |

## SIGNAL bank — Source (rail section): SDRplay service restart

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1292 | RESTART SDRPLAY SERVICE | button | DEFERRED | not found in signal_bank.slint: signal_logic builds the key (sdrplay_key_shown / _label / _enabled) but the Slint SourceModel has no field for it and SourceBody draws no such key; the elevated restart is a local helper |
| 1293 | Restart progress | readout | DEFERRED | not found in signal_bank.slint: the progress sentence (sdrplay_line) has no field in the Slint SourceModel |
| 1294 | Restart outcome | readout | DEFERRED | not found in signal_bank.slint: the outcome sentence (sdrplay_line) has no field in the Slint SourceModel |

## SIGNAL bank — Turn on the bias tee? (modal dialog)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1295 | Dialog | dialog | PRESENT | main.slint Sheet 'Turn on the bias tee?' (rows 107-109), opened by the strip's BIAS TEE key or the Source Bias tee tick; it does not close itself when the radio changes (only Esc, Cancel and the backdrop close it) |
| 1296 | Radio name | readout | PRESENT | the first words of the sheet's question line (the device name, set from the state) |
| 1297 | Warning | readout | PRESENT | the 4.5 V warning in the warning colour |
| 1298 | Memory line | readout | PRESENT | one sentence ('FoxSDR will switch it on again whenever this radio is opened'); the variant for a radio with no serial (not remembered) is not told apart |
| 1299 | Turn it on | button | PRESENT | Key -> FOXAPI_OP_SET_BIAS_TEE 1 (row 108) |
| 1300 | Cancel | button | PRESENT | the lit default key closes the sheet (row 109) |
| 1301 | Deck BIAS TEE key | key (latched toggle) | RELOCATED | the strip's BIAS TEE hazard key with its amber lamp (rows 21-22); enabled only when the radio has one |

## SIGNAL bank — Radio setup page (first-run / driver / find my radio page)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1302 | Page | dialog (page) | PRESENT | RadioSetupSheet: a 580 x 520 sheet over the window, titled RADIO SETUP, with a close key; fixed in place (no drag, roll-up, maximise or resize grip); shown by the model's flag (RADIO SETUP, and the host at launch unless the tick is set) |
| 1303 | Checking line | readout | PRESENT | amber Note (busy) above the lines |
| 1304 | Intro line | readout | PRESENT | the first line of the sheet, larger ('FoxSDR found no radio it can use ...' / 'This is what this computer shows ...') |
| 1305 | Finding list | list | PRESENT | per finding the title, explanation and install lines, plus one block per unbound radio (name, family and USB ids, advice); plain text, the usable (green) and not-checked (grey) tones are not told apart |
| 1306 | INSTALL DRIVER | button | PRESENT | one Key per finding with an install target, greyed while an install runs, with the tooltip -> setup-key('install-driver:<target>'), the host's local effect (core/winusb_install, the Windows prompt); signal_publish was not in the tree when this was ticked, so the launch itself is unverified |
| 1307 | Install-by-hand text | readout | PRESENT | the finding's install line in the list |
| 1308 | Vendor link | button (small) | PRESENT | small Key per finding, text = the URL without https://, tooltip = the URL -> setup-key('link:<url>') (local: the browser) |
| 1309 | Install status | readout | PRESENT | the install status line is appended to the list as a plain line (the amber tone is not applied) |
| 1310 | CHECK AGAIN | button | PRESENT | Key -> FOXAPI_OP_SCAN_DEVICES and the host restarts the USB probe; greyed while checking |
| 1311 | Don't show this again | checkbox | PRESENT | Tick -> setup-dont-show (saved by the host) |

## SIGNAL bank — Radio (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1312 | Radio header | key (fold) | RELOCATED | the Section row 'Radio' of bank 0 folds the body; its chip is the mode name |
| 1313 | Mode keys (8) | key (latched toggle) | RELOCATED | the strip's segmented control, folding to one picker key below 1760 px (row 72); Ctrl+F .. Ctrl+R |
| 1314 | VFO | slider | RELOCATED | no offset slider in the Radio body; the tuned band on the spectrum (click or drag -> FOXAPP_OP_VFO_TO_ABSOLUTE) and the counter set the VFO (a judgement: the +-500 kHz slider itself is not drawn) |
| 1315 | Bandwidth | dropdown/combo | RELOCATED | the Radio body's Passband slider in place of the width list (3 kHz to 0.9 of the channel rate -> FOXAPI_OP_SET_BANDWIDTH); dragging the band's edge on the spectrum sets it too |
| 1316 | Squelch | slider | RELOCATED | the strip's SQUELCH lane, -120..0 dB, with its AUTO key (row 44) |
| 1317 | De-emph | dropdown/combo | DEFERRED | not found in signal_bank.slint (nor in the Radio body of main.slint): no FM de-emphasis choice is drawn |
| 1318 | Signal strength | meter | DEFERRED | not found in signal_bank.slint (nor in the Radio body of main.slint): the strip's level bar is the audio level, not the signal power |
| 1319 | Stereo | checkbox | PRESENT | RadioExtras Tick with the tooltip -> FOXAPI_OP_SET_STEREO; WFM only, under the Radio body |
| 1320 | Stereo state | lamp/indicator | PRESENT | a Chip beside the tick (stereo green, forced mono amber, mono or no pilot dim) |
| 1321 | RDS block | readout | PRESENT | Caption RDS, PI and PS readouts and the radio text; WFM only, once RDS data is valid |

## SIGNAL bank — Audio filters (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1322 | Audio filters header | key (fold) | RELOCATED | the Section row 'Audio filters' of bank 0 folds the body; chip ON / OFF |
| 1323 | Chain note | readout | PRESENT | dim Note (chain-note) |
| 1324 | Noise reduction | checkbox | PRESENT | Tick -> FOXAPI_OP_SET_NR |
| 1325 | Strength | slider | PRESENT | SliderRow 0.00..1.00 -> FOXAPP_OP_SET_NR_STRENGTH; greyed unless Noise reduction is on |
| 1326 | Notch | checkbox | PRESENT | Tick -> FOXAPI_OP_SET_NOTCH |
| 1327 | Freq | slider | PRESENT | SliderRow, logarithmic 10 Hz to 20 kHz -> FOXAPP_OP_SET_NOTCH_FREQUENCY; greyed unless Notch is on |
| 1328 | Q | slider | PRESENT | SliderRow 1..200 -> FOXAPP_OP_SET_NOTCH_Q; greyed unless Notch is on |
| 1329 | Auto notch | checkbox | PRESENT | Tick -> FOXAPI_OP_SET_AUTO_NOTCH |
| 1330 | Auto notch status | readout | PRESENT | Note beside the tick, green when locked (on 1000 Hz), dim while searching; only while Auto notch is ticked |

## SIGNAL bank — Sinks (rail section: audio output)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1331 | Sinks header | key (fold) | RELOCATED | the Section row 'Sinks' of bank 0 folds the body; chip OPEN / CLOSED |
| 1332 | Device | dropdown/combo | PRESENT | DropKey -> FOXAPI_OP_AUDIO_DEVICE; replaced by the dim 'none' line when there is no device |
| 1333 | Audio health note | readout | PRESENT | amber Note (health-note) |
| 1334 | Audio buffer | dropdown/combo | PRESENT | DropKey AUTOMATIC / 120 / 240 / 480 / 960 ms with the tooltip -> FOXAPP_OP_SET_AUDIO_BUFFER |
| 1335 | Lead notice | readout | PRESENT | amber Note (lead-notice) |
| 1336 | Muted by | readout | PRESENT | amber Note 'Muted by <plugin>' with the dim note under it |
| 1337 | Sound is muted (popup) | dialog | RELOCATED | main.slint's Sheet 'Sound is muted' (the plugin kit's, Stop and resume / Keep it running -> KitEvents); phase 1 rows 104-106 predate it |

## SIGNAL bank — Transmit (rail row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1338 | Transmit switch row | key (latched toggle) | PRESENT | the Section row is a switch, not a fold: section-clicked on bank 0 row 5 calls TransmitPublisher::togglePage (transmit_publish.cpp), which opens or closes the kit's page kind 11; the row is lit while the page is open; it keys nothing (drive: transmit-page.txt) |
| 1339 | Transmit chip + lamp | lamp/indicator | PRESENT | the Section row's chip (NO TX / READY / ON AIR, red when keyed) and the red fault line under it (TransmitModel.fault = the transmitter error); the logic's FAULT word is not in sectionsFor today |

## SIGNAL bank — TRANSMIT page

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1340 | RADIO label + address | text field | PRESENT | ui/transmit_page.slint TransmitPage (kit page kind 11, opened torn off at 560 x 640 like the ImGui window): the RADIO Caption and the address LineInput; each edit is FOXAPP_OP_SET_TRANSMIT_ARGS (TransmitPage::addressEdited); greyed while a radio is open; the engine's address seeds the draft whenever the box is not focused |
| 1341 | Open | button | PRESENT | Key Open: FOXAPI_OP_TX_OPEN of the draft (TransmitPage::open); greyed for a blank box, tooltip 'Type the board's address first.'; a bounded run withholds it (boundedMaySend) |
| 1342 | Close | button | PRESENT | Key Close (replaces Open while a radio is open): FOXAPI_OP_TX_CLOSE; greyed while the key is down |
| 1343 | Radio name / state | readout | PRESENT | NoteLine radio-line: the open radio's name, else the engine's transmitter error (red, translated), else 'No transmitter is open.' (drive: transmit-page.txt reads it) |
| 1344 | Frequency | number field | PRESENT | LineInput 'MHz' (suffix) with the - / + keys (1 kHz, the ImGui InputDouble's step; its Ctrl fast step is not carried): FOXAPI_OP_TX_SET_FREQUENCY, live only with a radio open and SPLIT on; shows where it would transmit (txFrequencyHz) |
| 1345 | SPLIT | button (latched, red when on) | PRESENT | ToggleKey SPLIT (red ground when on, accessible checked): FOXAPI_OP_TX_SET_SPLIT with the receiver's dial and the flipped state; greyed with no radio |
| 1346 | Follow note | readout | PRESENT | the 'following the receiver' text beside SPLIT while SPLIT is off |
| 1347 | Mode keys | key (latched toggle, one lit) | PRESENT | SegGroup of the five mode SegKeys CW AM NFM USB LSB (radio-button, the lit one checked): FOXAPI_OP_TX_SET_MODE; the tooltip of each is its caption sentence; greyed with no radio |
| 1348 | TX POWER | slider | PRESENT | Slider in the board's own span, right = louder (txPowerFraction / txPowerFromFraction, the classic's functions) with the 'QUIET' / '-20.0 dB' readout and '(the board published no range)': FOXAPI_OP_TX_SET_POWER |
| 1349 | INPUT | label | PRESENT | Caption INPUT |
| 1350 | MIC | key (latched toggle) | PRESENT | SegKey MIC: FOXAPI_OP_TX_SET_INPUT 0; greyed with no radio and while the microphone is opening (the page therefore cannot open a microphone with no radio, which the table would) |
| 1351 | TONE | key (latched toggle) | PRESENT | SegKey TONE: FOXAPI_OP_TX_SET_INPUT 1 |
| 1352 | Tone frequency | number field | PRESENT | LineInput 'Hz' (suffix), shown on the TONE input: FOXAPI_OP_TX_SET_TONE, committed on Enter or on leaving the box |
| 1353 | AUDIO | meter | PRESENT | LevelBar AUDIO from txInputPeak, a fast rise and a slow fall (decay 0.90 a tick), on the classic's dB scale (txMeterFraction) |
| 1354 | PTT | button (hold) | PRESENT | HoldKey PTT (160 x 56): a HOLD - the pointer's level (a TouchArea keeps the press wherever the pointer goes) and, while the key has the keyboard focus, the bound chord (Space by default; row 138), turned into TX_PTT 1 every tick it is held and TX_PTT 0 once on release (the classic's txKeyCommands); greyed with no radio; NO accessible default action. UNVERIFIED against a real transmitter: none on this machine (docs 6.58) |
| 1355 | LATCH | button (latched, red when on) | PRESENT | ToggleKey LATCH (red ground when latched, accessible checked): a press is the classic's edge protocol (LATCH 0 then LATCH 1 on a key shown released, LATCH 0 on a key shown latched, in one batch); the state is the engine's |
| 1356 | State word | lamp/indicator | PRESENT | the lamp word beside LATCH (NO RADIO / READY / ON AIR / FAULT, amber READY, red ON AIR and FAULT, the classic's txLampState) and the Transmit row's chip (row 1339) |
| 1357 | Listen while transmitting | checkbox | PRESENT | Checkbox: FOXAPI_OP_TX_SET_MONITOR; greyed with no radio |
| 1358 | Auto-unkey / error | readout | PRESENT | NoteLine key-note (amber: why a LATCH or the consent was refused, the engine's words), unkey-reason (amber: why the key last opened on its own) and error-line (red: the transmitter's error) |
| 1359 | ARM FOR PLUGINS | label + tooltip | PRESENT | Caption ARM FOR PLUGINS with the classic's tooltip |
| 1360 | 5 MIN / 15 MIN / 60 MIN | key (latched toggle) | PRESENT | Keys 5 MIN / 15 MIN / 60 MIN (the armed length lit): a press is FOXAPP_OP_TX_PLUGIN_ARM 5 / 15 / 60 through the classic's txPluginArmCommands (with the lease's beat); live with no radio, where the engine's refusal comes back as fact.tx.plugin.refusal (drive: transmit-page.txt reads it) |
| 1361 | OFF (arming) | button | PRESENT | Key OFF: FOXAPP_OP_TX_PLUGIN_ARM 0; the page not being live sends OFF with reason 1 (the lease) |
| 1362 | Armed status | readout | PRESENT | NoteLine: 'ARMED: <plugin> may key this transmitter, 4:32 left.' (amber, red with 'ON AIR for <time>.' while a plugin has the key), else 'Not armed: no plugin can key this transmitter.', the refusal sentence (red) and the last close reason (amber) |
| 1363 | DATA (flat) | checkbox | PRESENT | Checkbox DATA (flat): FOXAPP_OP_TX_DATA_FLAT, live whatever the radio; drawn from fact.tx.plugin.dataFlat (drive: transmit-page.txt) |
| 1364 | Licence notice | readout | PRESENT | NoteLine: the classic's licence sentence (gui::kTxLicenceNotice) |

## SIGNAL bank — Recorder (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1365 | Recorder header | key (fold) | RELOCATED | the Section row 'Recorder' of bank 0 folds the body; chip REC (red) / OFF |
| 1366 | Record folder | readout | PRESENT | mono dim path (fact.recorder.dir) at the top of RecorderBody |
| 1367 | Record IQ | button (full width) | PRESENT | Key, danger look while recording -> FOXAPI_OP_RECORD_IQ 1; side by side with Record audio |
| 1368 | Starting IQ | button | PRESENT | the same key reads 'Starting IQ - click to cancel' (a press withdraws: RECORD_IQ 0), then 'Cancelling IQ start...' greyed; the 'disk is slow' line shows after 1 s |
| 1369 | Saving (IQ) | button (disabled) | DEFERRED | not found in signal_bank.slint: RecorderModel has no finalising state, so the key reads 'Record IQ' while the last take is still being written |
| 1370 | Stop IQ | button | PRESENT | the same key reads 'Stop IQ' -> FOXAPI_OP_RECORD_IQ 0 |
| 1371 | IQ progress | readout | PRESENT | mono dim readout ('IQ 3.2 s / n samples / x MB') while recording |
| 1372 | Record audio | button (full width) | PRESENT | Key -> FOXAPI_OP_RECORD_AUDIO 1 |
| 1373 | Starting audio | button | PRESENT | the same key reads 'Starting audio - click to cancel', then 'Cancelling audio start...' greyed |
| 1374 | Saving (audio) | button (disabled) | DEFERRED | not found in signal_bank.slint: RecorderModel has no finalising state, so the key reads 'Record audio' while the last take is still being written |
| 1375 | Stop audio | button | PRESENT | the same key reads 'Stop audio' -> FOXAPI_OP_RECORD_AUDIO 0 |
| 1376 | Audio progress | readout | PRESENT | mono dim readout while recording |
| 1377 | Muted warning | readout | PRESENT | amber Note 'Muted by <plugin> - this take is silent' |
| 1378 | Press Play hint | readout | PRESENT | dim Note '(press Play to feed the recorders)' |
| 1379 | Recorder error / notice | readout | PRESENT | red Note (record error, 'disk refused the file') and a dim Note (the 4 GB limit, 'the file was closed and is complete') |

## SIGNAL bank — Frequency-list import dialog

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1380 | Dialog | dialog | PRESENT | ImportDialog: a 460 px sheet with a close key; the backdrop, Cancel and Esc close it (docs 6.61: SheetFrame's key scope takes the keyboard while the sheet is up and dismisses on Esc; the root handler's escapeStep names the dialog first); drive escape-leaves-dialog.txt |
| 1381 | Reason line | readout | PRESENT | Note at the top (import-reason, error) |
| 1382 | Preset note | readout | PRESENT | ImportDialogModel.preset (docs 6.61): an import opened for an Airband preset says 'The rows will join the preset "X".' where 'Import as' stood, and offers no I/Q file; a path typed in it is the section's own preset import (test_slint_signal renders it; drive tools-airband-import.txt reads the rows landing in the preset) |
| 1383 | Drop zone | drag/gesture | DEFERRED | drawn as words only; no file-drop event is wired into the Slint window (not found in signal_bank.slint, main.slint or main.cpp), so a dropped file imports nothing; the path field is the way in |
| 1384 | or / Browse... | button | DEFERRED | the OS file chooser: a path field takes its place (Enter imports: FOXAPP_OP_BOOKMARK_IMPORT_FILE for a frequency list, SELECT_SOURCE 'file:' for an I/Q recording); the Browse key is drawn and calls import-open, the host's local pickFile |
| 1385 | Reading line | readout | DEFERRED | not found in signal_bank.slint: reading_line is a host extra with no field in the Slint ImportDialogModel |
| 1386 | Cancel | button | PRESENT | Key -> import-closed (local) |

# Phase 3: the SYSTEM bank (docs 6.40) - `inventory/extend-system-store-patch.txt` lines 287-1380 walked row by row

Every control row of the SYSTEM bank of `inventory/extend-system-store-patch.txt` (FoxSDR 0.99.104; lines 287-1380: Display, Language & country, Frequency correction, Updates and its banner, Serial ports, Key bindings, Diagnostics with its two dialogs, Usage reporting, Beta tester with its link prompt, and the two full-page screens Request a feature and Report a bug or dislike; 137 rows) against the SYSTEM bank of the Slint window: the bodies of `src/slint/ui/system_bank.slint` (`DisplayBody`, `LanguageBody`, `UpdatesBody`, `SerialPortsBody` with `GpsRowView`, `KeyBindingsBody`, `DiagnosticsBody`, `UsageBody`, `TesterBody`, `ReportSheet`), their effects (`src/slint/system_logic.hpp`, the callbacks-to-effects table in its header) and the places `src/slint/ui/main.slint` puts them (bank 4, rows 0-8: Display, Language & country, Frequency correction, Updates, Serial ports, Key bindings, Diagnostics, Usage reporting, Beta tester; row 9, About, opens the About sheet; `ReportSheet` is an overlay shown by `report-page.shown`). The status words are phase 1's: **PRESENT** = the family draws it and its callback has an effect in `system_logic` (a saved setting, an op, a local state change or a report; the note names the component and the effect), **RELOCATED** = it lives elsewhere in the Slint window (the Section row, the update Card the host duties draw, the status bar's keys, the Frequency correction body shared with SIGNAL), **DEFERRED** = not drawn, drawn without an effect, or saved but not applied, with the reason. A control whose status could not be established from the files is DEFERRED with "not found in system_bank.slint". The Display theme's six looks all map onto the one Workbench skin today, so a pick is saved and not applied; the interface size is saved and nothing in the tree applies it (unverified whether it will be live or at the next start). The chips of the seven sections below Frequency correction are described in the inventory's headings, not as rows: they are `SystemModels.chips` (system_logic), and the SYSTEM rows of `main.cpp` (`sectionsFor`) still draw the earlier eight names with a static '--' chip, so the ten rows `main.slint` places the bodies under (and the About row at index 9) wait for the publisher. The family also draws controls the inventory does not list: the 'Open the log' key (Diagnostics) and the SHOW / HIDE key on the tester code. The 'Updates footnotes' row at inventory line 1586 belongs to the Plugin store's Updates tab, not to this bank, and is not ticked here. The host's publisher (`system_publish`, named in `main.slint`) was not in the tree when this part was ticked: PRESENT here means drawn and given an effect in `system_logic`, not seen working in the window.

## SYSTEM bank — Display

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1387 | Section chip | readout + lamp | RELOCATED | the Section row 'Display' of bank 4 carries the chip (PLAN / PLAIN) and the lamp (lit while the overlay draws): system_logic readSystem fills SystemModels.chips[0]; main.cpp sectionsFor still draws a static '--' chip and no dot until the SYSTEM publisher feeds the rows |
| 1388 | Interface size | dropdown/combo | PRESENT | DropKey (Auto + six steps) -> display-scale -> applyDisplayScale: save interfaceScale; main.cpp applyLook (look_logic ScaleFollower) reads the interface half every tick and dispatches the scale to the window (Window::dispatch_scale_factor_change_event), keeping the logical size by resizing the window unless it is maximised: a maximised window lays out in 1/scale as many logical pixels, as the classic's scale does. 'Auto' follows the system's own number, remembered apart from the one dispatched (the Auto row's '% here' quotes it). Live and at the first tick, no restart (drive system-look.txt: 200% = scale 2.00 and 800 x 450 logical, back to Auto = 1.00; Ctrl+0) |
| 1389 | Interface size tooltip | tooltip | PRESENT | the DropKey's tooltip (scale-tooltip, the ImGui hint through tr()) |
| 1390 | Theme | dropdown/combo | PRESENT | DropKey of the six looks -> display-theme -> applyDisplayTheme: save uiTheme plus the preset's counter, readings and tuner style; main.cpp applyLook reads them every tick: 'night' installs the NightWatch skin, every other key Workbench, and the counter face follows the saved tuner style (docs 6.61; drive system-look.txt reads look.bg, look.accent, look.face for Night Watch, Today's bench and Glass Cockpit) |
| 1391 | Min dB | slider | PRESENT | SliderRow (readout db-min-text) -> display-db-min (0..1 of the track) -> applyDisplayDbMin: save dbMin / dbMax with the other end pushed back (clampManualRange) and waterfallRangeMode manual, op FOXAPI_OP_SET_DISPLAY_RANGE; the publisher maps track to dB (unverified) |
| 1392 | Max dB | slider | PRESENT | SliderRow -> display-db-max -> applyDisplayDbMax: the same save and FOXAPI_OP_SET_DISPLAY_RANGE with the ceiling as the end that was moved |
| 1393 | Frequency display | dropdown/combo | PRESENT | DropKey Nixie / Neon / Plain -> display-freq-display -> applyDisplayFreqDisplay: save tunerDisplayStyle; the strip's face is main.cpp applyFace, driven by the counter menu (row 155) and not read back from the saved half yet (unverified until the publisher) |
| 1394 | Counter figures | dropdown/combo | DEFERRED | DropKey Normal size / Enlarged figures -> applyDisplayFigures: save counterScale; the Slint counter has no scale (main.cpp logs the request as phase 2's, phase-1 row 156) |
| 1395 | Tuner switches | checkbox | PRESENT | Tick with the tooltip (switches-tooltip) -> display-tuner-switches -> applyDisplayTunerSwitches: save counterSwitches; the nudge arrows follow main.cpp showNudges (counter menu, row 157), which does not read the saved half yet (unverified) |
| 1396 | Enlarge every reading | checkbox | DEFERRED | Tick -> display-enlarge -> applyDisplayEnlarge: save readingsScale 1.4 / 1.0; no Slint readout scales with it (phase-1 row 158) |
| 1397 | Aircraft icons | slider | PRESENT | SliderRow 16-96 px (icon-text '48 px', icon-tooltip) -> display-icon-px -> applyDisplayIconPx: save aircraftIconPx and pull mapTrailWidthPx down to the wingspan; the right-click reset is not drawn (Slider has no right-click gesture) although the tooltip still says 'Right-click to put it back' |
| 1398 | Trail width | slider | PRESENT | SliderRow (trail-text, trail-tooltip) -> display-trail-px -> applyDisplayTrailPx: save mapTrailWidthPx (1 .. the icon's wingspan, trail_max a host extra with no Slint field); right-click reset not drawn, as above |
| 1399 | Band plan | checkbox | PRESENT | Tick -> display-band-plan -> applyDisplayBandPlan: save bandPlanOverlay; always drawn |
| 1400 | Size (band plan) | dropdown/combo | PRESENT | DropKey Small / Medium / Large, on one line with Colour, drawn only while Band plan is ticked -> display-band-size -> applyDisplayBandSize: save bandPlanSize |
| 1401 | Colour (band plan) | dropdown/combo | PRESENT | DropKey Classic / Vivid / Mono, beside Size, drawn only while Band plan is ticked -> display-band-colour -> applyDisplayBandColour: save bandPlanPalette |
| 1402 | Region | dropdown/combo (searchless list) | PRESENT | DropKey drawn only with the overlay ticked and more than one plan (regions.length > 1); closed text region-preview or '(all installed)' -> display-region -> applyDisplayRegion: op FOXAPI_OP_SET_BAND_PLAN with the plan id, nothing for the plan in force |
| 1403 | Plan status | readout | PRESENT | Note plan-status in its tone (red for the error) while the overlay is ticked |

## SYSTEM bank — Language & country

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1404 | Language | dropdown/combo | PRESENT | LanguageBody DropKey (Automatic first, each language by its own name, undrawable ones greyed through SelectItem.enabled and, since 6.62, given their reason beside the name) -> language-selected -> applyLanguageSelected: save language, apply Language (setLanguage + select_bundled_translation + the CJK face: cjk_fonts.cpp chooses the system face as the classic does, Theme.cjk-family names it as the window's typeface, so ja / ko / zh-CN / zh-TW draw with no missing-glyph boxes; a machine with no such face greys the language: cascade::i18n::drawable through languageDrawable); closed text language-text |
| 1405 | Machine-translation hint | readout | PRESENT | amber Note machine-hint, only while the language in force is flagged machine |
| 1406 | SUGGEST A BETTER TRANSLATION | button | PRESENT | small Key (translation-label), drawn with the hint -> language-translation -> applyLanguageTranslationReport: local, opens the bug page with the draft 'Translation (<code>): ' unless the person has one (the page itself: rows of 'Report a bug or dislike') |
| 1407 | Country | dropdown/combo (tall) | PRESENT | DropKey with a filter field above its list -> country-selected -> applyCountrySelected: save country and op FOXAPI_OP_SET_BAND_PLAN for the country's plan when it is not the one in force; closed text country-text |
| 1408 | Country filter | text field | PRESENT | LineInput at the top of the open list (grab-focus, placeholder country-hint) -> country-filter on each edit (local, ReadOptions::country_filter); a pick sends country-filter('') so the list is whole again |
| 1409 | Not set | list item | PRESENT | the first row of the countries list while the filter is empty (built by the host) -> country-selected -> country saved as '', the band plan left alone |
| 1410 | Band-plan line | readout | PRESENT | Note plan-line ('Band plan: <name>') once a country is chosen |
| 1411 | Plan-changed hint | readout | PRESENT | Note plan-changed-hint (host extra; ReadOptions::plan_changed_note says when) |
| 1412 | USE <LANGUAGE> | button | PRESENT | small Key (suggest-label, 'USE <LANGUAGE>') -> language-suggest -> applyLanguageSuggest: save language = the country's catalogue, apply Language; not drawn when the label is empty |

## SYSTEM bank — Frequency correction

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1413 | Section chip | readout | RELOCATED | the Section row 'Frequency correction' (row 2 of bank 4 in main.slint); its chip (OFF / ON / PPM +1.5) and lamp are the SIGNAL publisher's ppm model (signal_logic ppm_chip), SystemModels.chips leaves it empty; main.cpp's SYSTEM row list has no such row yet |
| 1414 | PPM frequency correction | checkbox | RELOCATED | FrequencyCorrectionBody at SYSTEM row 2 of main.slint, the SIGNAL family's body on the same source-model (row 1286) |
| 1415 | Not-applicable note | readout | RELOCATED | the same body (row 1287) |
| 1416 | Whose-radio line | readout | RELOCATED | the same body (row 1288) |
| 1417 | PPM value | number field | RELOCATED | the same body (row 1289) |
| 1418 | How-applied note | readout | RELOCATED | the same body (row 1290) |
| 1419 | Refusal note | readout | RELOCATED | the same body (row 1291) |

## SYSTEM bank — Updates

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1420 | Check for updates at startup | checkbox | PRESENT | UpdatesBody Tick (check-label carries the Store copy's wording) -> updates-check -> applyUpdatesCheck: save updateCheckEnabled, apply UpdateCheck (HostDuties::updateSettingChanged: off discards the answer, on asks again) |
| 1421 | Stable releases | radio | PRESENT | RadioChoice -> updates-channel(0) -> applyUpdatesChannel: save updateChannel stable, re-check when the check is on; not drawn for a Store copy (packaged) |
| 1422 | Every release (beta) | radio | PRESENT | RadioChoice -> updates-channel(1) -> applyUpdatesChannel: save updateChannel beta, re-check when the check is on; not drawn for a Store copy |
| 1423 | Channel help | readout | PRESENT | Note channel-help; not drawn for a Store copy |
| 1424 | Privacy text | readout | PRESENT | Note privacy (the foxsdr.com and the Microsoft Store wordings) |
| 1425 | Version line | readout | PRESENT | Note version-line ('this build: x'; a Store copy adds 'package: ...') |
| 1426 | Team message count | readout | PRESENT | Note team-message ('N message(s) from the FoxSDR team': site copy, check on, unseen > 0, the count from HostDuties::updateStatus), pressable -> updates-team-message (local); the cards it should bring forward are deferred (rows below), so the press shows nothing |
| 1427 | Status line | readout | PRESENT | Note status-line in its tone (amber update, red important): checking is off / checking... / <version> is available / up to date / last check did not complete: <error>; the Store copy's 'the Microsoft Store has an update' needs the Store flow's facts, which the duties defer |
| 1428 | Show it again | button (small) | PRESENT | small Key (show-again-label) -> updates-show-again -> applyUpdatesShowAgain: apply UpdateShowAgain (HostDuties::updateShowAgain); drawn only while an update is offered and its card was dismissed |

## SYSTEM bank — Update banner and team messages (top of the rail column, every bank)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1429 | Team message card | card | DEFERRED | the Card exists in main.slint, but the host duties leave the team message cards out: main.cpp keeps team_message_shown false (phase-1 row 87) |
| 1430 | Seen | button | DEFERRED | on_team_message_seen is an empty lambda in main.cpp and the Seen list (the interface half's messagesSeen) is not written by the Slint host (phase-1 row 88) |
| 1431 | Update banner (site) | card | RELOCATED | the update Card at the foot of the sections panel, drawn by the host duties (updateCardFor / applyUpdateCard); phase-1 rows 89-91 |
| 1432 | Download update | button | RELOCATED | the Card's primary key with the progress bar and the size note (phase-1 rows 92-93) |
| 1433 | Install now and restart | button | RELOCATED | the Card's primary key after the verified download (phase-1 row 94) |
| 1434 | Open foxsdr.com | button | RELOCATED | the Card's primary key on a non-Windows build (phase-1 row 95) |
| 1435 | Not now | button (small) | RELOCATED | the Card's secondary key (update-dismissed; phase-1 row 96); Show it again in the Updates body brings the Card back |
| 1436 | Update banner (Store) | card | DEFERRED | the duties hide the Card for a Store copy (updateCardFor: packaged) and log the Store flow as deferred (UpdatePlan::StoreDeferred); no Store banner is drawn |
| 1437 | Install from the Microsoft Store | button | DEFERRED | the Store's install flow is deferred in host_duties (phase-1 row 102); no key, and UpdatesFacts carries no Store facts |

## SYSTEM bank — Serial ports

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1438 | Intro | readout | PRESENT | Note intro |
| 1439 | Refresh | button | PRESENT | small Key -> serial-refresh -> opSerialRefresh: FOXAPI_OP_SCAN_SERIAL_PORTS (the answer is the SERIAL_PORTS list changing) |
| 1440 | Empty state | readout | PRESENT | Note empty-line, drawn when the list is empty; system_logic leaves it '' until the scan has answered |
| 1441 | Port list | table (1 column, borders, row bg) | PRESENT | one-column table drawn directly in SerialPortsBody (frozen 'Port' heading on the strip ground, 24 px mono rows with a hairline under each); the model is [string] |
| 1442 | GPS receiver | readout (separator) | PRESENT | the rule, the caption and GpsRowView: port DropKey -> gps-port (save gpsPort, clear the two GPS status slots with FOXAPP_OP_CLEAR_STATUS), baud DropKey -> gps-baud (save gpsBaud), the lit read / Stop key -> gps-toggle (FOXAPI_OP_GPS start with port and baud, or stop), then the status, satellites and refusal lines; the Radar section, scope and map-bar copies of the row are other bodies' and not part of this bank |
| 1443 | GPS note | readout | PRESENT | Note gps-note |

## SYSTEM bank — Key bindings

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1444 | Help text | readout | PRESENT | Note help |
| 1445 | Reset to defaults | button | PRESENT | xs Key on the first group's heading line -> keys-reset -> applyKeysReset: save keyBindings = {} (the stock table), the capture ends |
| 1446 | Group heading | readout | PRESENT | Caption per group (the first beside Reset, later ones under a thin rule), a new heading each time the group changes as readKeyBindings builds them |
| 1447 | Action name | readout | PRESENT | Text per KeyRow (action), 32 rows from the host's copy of the action table (system_logic kActions, held equal to gui/key_bindings.cpp by test_slint_host_system) |
| 1448 | Key cap (one per action) | key (momentary, capture) | PRESENT | KeyCap, one 112 px pill for every row: press -> keys-capture(id) -> local capture (applyKeysCapture); 'Press a key...' while listening; the next chord -> keys-captured -> applyKeysCaptured: save keyBindings; Esc -> keys-cancel, Backspace -> keys-clear, a second press cancels. Two limits: a Ctrl+Alt chord cannot be captured (Slint on Windows delivers it without modifiers), and the root FocusScope of main.slint still dispatches the stock chords only, so a saved rebinding is not honoured until it reads the table |
| 1449 | Clash note | readout | PRESENT | red Note 'also <action>' under the row (clash from clashOf); the ImGui rule 'first in list wins at dispatch' holds: the host answers the first action in table order (key_dispatch, docs 6.56) |
| 1450 | Footer | readout | PRESENT | Note footer: the ImGui hint (F12 always saves a screenshot, Esc always cancels); the counts 'N actions, M clashes' ride in system_logic (action_count, clash_count) but the Slint KeyBindingsModel has no field for them |

## SYSTEM bank — Diagnostics

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1451 | Explanation | readout | PRESENT | DiagnosticsBody Note explanation |
| 1452 | Record crashes and freezes, and send the report next time | checkbox | PRESENT | Tick -> diag-record -> applyDiagRecord: save diagnosticsEnabled, apply Diagnostics (HostDuties::diagnosticsChanged arms or disarms the crash capture; the half goes out at once) |
| 1453 | What is / is not sent | readout | PRESENT | Note sent-text under the Record tick |
| 1454 | Also write a full memory dump beside a crash report | checkbox | PRESENT | Tick -> diag-minidump -> applyDiagMinidump: save diagnosticsMinidump, apply Diagnostics; the dump-note sits under it |
| 1455 | Log path | readout | PRESENT | MonoNote log-path ('Log: <path>' or 'Log: off ...'); the family adds an 'Open the log' key -> diag-open-log (local, applyDiagOpenLog) that the inventory does not list |
| 1456 | Reports path | readout | PRESENT | MonoNote reports-path ('Reports: <path>' or '(unavailable)') |
| 1457 | Show the pointer ledger across the top of the window | checkbox | DEFERRED | Tick drawn (diag-ledger, ledger, ledger-note) but system_logic has no effect or field for it: the pointer ledger is window-bound state of the ImGui build (header comment: not covered) |
| 1458 | Copy diagnostics | button | DEFERRED | small Key drawn (diag-copy, bundle-status) but system_logic has no effect for it: the clipboard and the diagnostics bundle are window-bound (header comment: not covered) |
| 1459 | Open reports folder | button | PRESENT | small Key -> diag-open-reports -> applyDiagOpenReports: local, opens the folder (text = reports_path) |
| 1460 | SDRplay intro | readout | DEFERRED | not found in system_bank.slint as a host source: sdrplay-text is a Slint field with no counterpart in system_logic's DiagnosticsModel, so the whole block (rule, text, key, status) stays hidden |
| 1461 | Run SDRplay diagnostic | button | DEFERRED | the Key (diag-sdrplay, sdrplay-enabled) is drawn only with sdrplay-text and has no effect in system_logic; no probe dialog follows (rows below) |
| 1462 | What exactly is in a report? | button (small) | PRESENT | xs Key -> diag-what -> applyDiagWhat: local flag shared with Usage reporting's what-open |
| 1463 | Report field list | readout | PRESENT | Note what-text while the flag is open |

## SYSTEM bank — SDRplay diagnostic dialog (window, 520 px, closable)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1464 | Warning text | readout | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog or bias confirmation is drawn (the Run SDRplay diagnostic key has no effect in system_logic, whose header lists the probe and its sheet as window-bound and not covered) |
| 1465 | Also test the bias tee | checkbox | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog is drawn |
| 1466 | Bias confirmation | dialog (inline) | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog is drawn |
| 1467 | Yes, switch it on | button | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog is drawn |
| 1468 | Cancel (bias) | button | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog is drawn |
| 1469 | Run the diagnostic | button | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog is drawn |
| 1470 | Cancel | button | DEFERRED | not found in system_bank.slint: no SDRplay diagnostic dialog is drawn |

## SYSTEM bank — "FoxSDR did not close normally last time" dialog (520 px)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1471 | Dialog | dialog | RELOCATED | an amber Note at the top of the Diagnostics body (DiagnosticsModel.offer, set while ReadOptions::last_run_unclean and diagnostics are on); no window at start-up, the duties write a log line instead (host_duties.hpp) |
| 1472 | Copy diagnostics | button | DEFERRED | the body's Copy diagnostics key has no effect (see the Diagnostics block) |
| 1473 | Open reports folder | button | RELOCATED | the Diagnostics body's Open reports folder key (PRESENT there), which sits under the offer note |
| 1474 | Not now | button | DEFERRED | not found in system_bank.slint: the offer note has no key or callback to dismiss it; it shows for as long as the offer is due |

## SYSTEM bank — Usage reporting

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1475 | Disclosure | readout | PRESENT | UsageBody Note disclosure |
| 1476 | Send anonymous usage reports | checkbox | PRESENT | Tick -> usage-send -> applyUsageSend: op FOXAPP_OP_TELEMETRY_CONSENT (grant ADMIN); the tick shows the value asked for until the fact says it |
| 1477 | Install id | readout | PRESENT | MonoNote install-id-line ('Install id: <id>'), only while on |
| 1478 | Off line | readout | PRESENT | Note state-line: 'Off. Nothing is transmitted.' while off (the same field carries the two-messages sentence while on) |
| 1479 | What exactly is sent? | button (small) | PRESENT | xs Key (what-label) -> usage-what -> applyUsageWhat: local flag shared with Diagnostics |
| 1480 | Privacy policy (foxsdr.com) | button (small) | PRESENT | xs Key (privacy-label) -> usage-privacy -> applyUsagePrivacy: local, opens core::kPrivacyPolicyUrl; the URL tooltip is not drawn (privacy_url has no Slint field) |
| 1481 | Payload list | readout | PRESENT | what-text Note and a field-ground box of the payload paragraphs, both while the flag is open |

## SYSTEM bank — Beta tester

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1482 | Explanation | readout | PRESENT | TesterBody Note explanation; the section is shown only when the host says so (TesterModel.shown / tester_revealed) |
| 1483 | Linked to | readout | PRESENT | Readout linked-line ('Linked to <name>', '(no name set)' when empty) while an app token is set |
| 1484 | Unlink | button (small) | PRESENT | xs Key beside it -> tester-unlink -> applyTesterUnlink: save testerAppToken and its name = '' |
| 1485 | Code line | readout | PRESENT | Readout code-line ('Code: abcd....wxyz', masked) while a legacy portal token is set |
| 1486 | Remove code | button (small) | PRESENT | xs Key beside it -> tester-remove-code -> applyTesterRemoveCode: save testerToken = '', invalid flag off, the app token and its name cleared with it |
| 1487 | Invalid-code message | readout | PRESENT | red Note invalid-line (the red error-line beside it is the sentence for a paste that is no code) |
| 1488 | Tester code | text field (masked) | PRESENT | masked mono LineInput (hidden) with a SHOW / HIDE key that the inventory does not list; Enter or focus loss -> tester-code-committed (local draft); drawn when field-shown |
| 1489 | Use this code | button | PRESENT | small Key: commits the draft, then tester-use -> applyTesterUse: a 32-hex portal token or a whole link is saved as testerToken; a 40-hex app token only reaches Local::TesterResolve because the Link prompt is deferred (rows below); anything else gives the error sentence |
| 1490 | What is sent | readout | PRESENT | Note sent-text |
| 1491 | Show / Hide what is sent | button (small) | PRESENT | xs Key, label swaps -> tester-show-sent -> applyTesterShowSent (local flag) |
| 1492 | Preview | readout | DEFERRED | the box is drawn and shows 'Nothing - no code is set.' and the queue line, but the masked JSON of the session's record comes from the tester usage recorder, which the Slint host leaves out (tester_preview_json stays empty), so a linked tester sees an empty preview |

## SYSTEM bank — "Link this FoxSDR?" window (460 px, closable)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1493 | Question | dialog | DEFERRED | not found in system_bank.slint: no Link prompt sheet; tester-use stops at Local::TesterResolve, and the confirm-by-name flow and its link poll are left out of the host duties |
| 1494 | Link | button | DEFERRED | not found in system_bank.slint: no Link prompt sheet; tester-use stops at Local::TesterResolve, and the confirm-by-name flow and its link poll are left out of the host duties |
| 1495 | Not now | button | DEFERRED | not found in system_bank.slint: no Link prompt sheet; tester-use stops at Local::TesterResolve, and the confirm-by-name flow and its link poll are left out of the host duties |

## SYSTEM bank — Full-page screens: Request a feature (page)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1496 | REQUEST A FEATURE key | button (rail/status column) | RELOCATED | the status bar's 'Request a feature' key in main.slint (feature-request-clicked); the page is ReportSheet kind 0 (report-open(0)); main.cpp's handler only logs 'companion screen (phase 3)' as read, so the key opens nothing until the publisher raises the sheet |
| 1497 | Prompt | readout | PRESENT | ReportSheet Text over the box (prompt) |
| 1498 | Message box | text field (multiline) | PRESENT | TextBox (multi-line, follows the caret) -> report-text on each edit (local draft); not greyed while sending (the Slint model has no sending field) and a fixed 56 px high, not shrinking with the window |
| 1499 | Counter | readout | PRESENT | Readout counter ('n / 100000 characters') |
| 1500 | Contact | text field | PRESENT | LineInput with its caption (contact-label) -> report-contact on each edit and on commit (local draft) |
| 1501 | No-email warning | readout | PRESENT | amber Note no-email-note |
| 1502 | What is sent | readout | PRESENT | Note attach-note (the two sentences, by contact) |
| 1503 | SEND | button (120 px) | PRESENT | Key (send-label) live when send-enabled and the box holds text -> report-send -> applyReportSend: a ReportSend for FeatureRequestSender, nothing unless the verdict is 'can send'; the reason '(write at least ...)' is the refusal Note under the key |
| 1504 | Status line | readout | PRESENT | Note sent-line in sent-tone (Sending... amber, Thank you green, failure red, cool-down amber); clearing the box on success while keeping the contact is the host's draft handling (unverified) |

## SYSTEM bank — Full-page screens: Report a bug or dislike (page)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1505 | REPORT A BUG / DISLIKE key | button (status column) | RELOCATED | the status bar's 'Report a bug / dislike' key in main.slint (bug-report-clicked) and Language's SUGGEST A BETTER TRANSLATION key; the page is ReportSheet kind 1 (report-open(1)); main.cpp's handler only logs as read, as for the feature key |
| 1506 | Kind question | readout | PRESENT | Text kind-question ('What are you reporting?'), bug page only |
| 1507 | Kind: bug | radio | PRESENT | RadioChoice (problem-kinds[0]) -> report-kind(0) -> applyReportKind: local, the attach box follows the kind's default (ticked); neither is chosen when the page opens (problem-kind-index -1) |
| 1508 | Kind: dislike | radio | PRESENT | RadioChoice beside it (problem-kinds[1]) -> report-kind(1) -> applyReportKind: local, the attach box unticked |
| 1509 | Prompt | readout | PRESENT | Text over the box; the host words it by kind (bug or dislike) |
| 1510 | Message box | text field (multiline) | PRESENT | TextBox, 64 px -> report-text on each edit (local draft); not greyed while sending, as on the feature page |
| 1511 | Counter | readout | PRESENT | Readout counter |
| 1512 | Pasted-report detector | readout | PRESENT | green Note recognised-line ('A report was recognised and attached as <class>') |
| 1513 | Contact | text field | PRESENT | LineInput with its caption -> report-contact (local draft) |
| 1514 | No-email warning | readout | PRESENT | amber Note no-email-note |
| 1515 | Attach the diagnostics log | checkbox | PRESENT | Tick (bug page only) -> report-attach -> applyReportAttach (local); default follows the kind |
| 1516 | Attach the SDRplay diagnostic too | checkbox | PRESENT | Tick, drawn when probe-shown (attach ticked and a probe report exists) -> report-probe -> applyReportProbe (local); no probe can run in the Slint window (Run SDRplay diagnostic is deferred), so it is never offered in practice |
| 1517 | Show what will be sent | checkbox | PRESENT | Tick, drawn while attach is ticked -> report-preview -> applyReportPreview (local) |
| 1518 | Attachment preview | text field (read-only, multiline, 140 px) | PRESENT | 140 px field-ground box with a Flickable of mono Text (preview-text): scrolls and is read-only, but it is Text, so the attachment cannot be selected or copied from it |
| 1519 | Local-only explanation | readout | PRESENT | Note local-only-line (the newest report's reason and where the file is) while the preview is open |
| 1520 | What is sent | readout | PRESENT | Note attach-note, the four variants of problemSentence (log x contact) |
| 1521 | SEND | button (120 px) | PRESENT | Key -> report-send -> applyReportSend: a ReportSend for ProblemReportSendFlow; the wrapped reason is the refusal Note under the key |
| 1522 | Status line | readout | PRESENT | Note sent-line in sent-tone |
| 1523 | Log-dropped note | readout | PRESENT | no note of its own: the sentence rides in sent-line after 'Thank you - your report was sent.' (green, not amber) |

# Phase 3: the TOOLS bank and the map rows (docs 6.41) - `inventory/tools-bank-map.txt` walked row by row

Every control of `inventory/tools-bank-map.txt` (FoxSDR 0.99.104; the file calls itself "about 235 controls and elements" and holds 251 control records, plus one row this part adds, see below: 252 rows) against the TOOLS bank and the map pages of the Slint window: the bodies of `src/slint/ui/tools_bank.slint` (`PositionOffersView`, `DemodScopeRow`, `RadarBody`, `BookmarksBody`, `ScannerBody`, `AirbandBody`), their effects (`src/slint/tools_logic.hpp`, the callbacks-to-effects table in its header) and the places `src/slint/ui/main.slint` puts them (bank 2, rows 1-4: Radar, Bookmarks, Scanner, Airband; row 0, Demod scope, is a row with no body). The map rows (the Map page, the track list, the canvas, the Satellites map window, the Map rows and Target details) are not in this bank's bodies: they are the plugin kit's pages (`pages.slint` `MapToolbar` and `PageBody` kind 5, `map.slint` `MapPane`, `kit_publish.cpp`), ticked in phase 2 (docs 6.38), and are RELOCATED here with the phase-2 row they correspond to. The status words are phase 1's: **PRESENT** = the family draws it and its callback has an effect in `tools_logic` (an op, a local change the publisher keeps, or both; the note names the component and the effect), **RELOCATED** = it lives elsewhere in the Slint window (the Section row `main.cpp` draws, the kit's map page, the SIGNAL family's import dialog, the SYSTEM family's GPS row, the spectrum's own bookmark marks, another form of the same control), **DEFERRED** = not drawn, drawn without an effect, or drawn but not placed, with the reason. A control whose status could not be established from the files is DEFERRED with "not found in" and the file. The DEMOD SCOPE window (22 rows), the RADAR SCOPE full-window mode (28 rows) and its LCD data panel (15 rows) were the bespoke scope modes and are built in docs 6.57 (the demod scope window, the radar scope in the SCOPE tab, its LCD panel; rows 1964-1968 are added for what the inventory did not list; the basemap tiles under the radar tube and the cabinet's case are DEFERRED). The host's publisher (`tools_publish`, named in `main.slint`) was not in the tree when this part was ticked, so PRESENT here means drawn and given an effect in `tools_logic`, not seen working in the window, and the five TOOLS Section rows are still the ones `main.cpp` (`sectionsFor`) draws: a static '--' chip on Demod scope, Radar and Airband, the live count on Bookmarks, SCANNING / OFF on Scanner, and no lamp on any (the chip words and lamps of `readTools` wait for the publisher). The Slint `Slider` calls its `changed` on every drag step, so the Airband squelch sends an op per step where the ImGui control sends on release (the publisher is not there to hold them back). One row is added to the inventory's: the Airband row's level tooltip is ticked apart from the row's click (the inventory's single Row line 1318), so that the click is not marked missing and the tooltip is not marked built. The family also draws what the inventory does not list: the Markers window as `MarkersSheet` (phase-1 rows 239-252, which still read DEFERRED as a companion window and are not edited here); it is not ticked here.

## TOOLS bank — TOOLS (left-rail bank key and its group caption)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1524 | Bank key | key (latched toggle, one of five) | PRESENT | RailKey TOOLS with the F3 hint (main.slint; bank-selected(2)); phase-1 row 63 |
| 1525 | Group caption | readout | PRESENT | the sections panel's head caption names the bank 'TOOLS' (bank-titles); phase-1 row 66 |

## TOOLS bank — Demod scope (rail row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1526 | Scope switch row | key (switch row, opens window) | PRESENT | the Section row 'Demod scope' (bank 2 row 0): a press -> ToolsEvents.scope-clicked -> applyScopeClicked (a [local] Deferred 'demod-scope') -> ToolsPublisher::Hooks::demodScopeClicked -> ScopePublisher::toggleDemod: the window opens and the second press shuts it (scope-demod-open.txt: state scope.demodOpen) |
| 1527 | Input chip | readout | PRESENT | ScopePublisher hands the input, the open flag and the tap's liveness to ToolsPublisher::setScopeFacts, so readScopeChip words the chip AUDIO / SPEC / I/Q / VECTOR / MPX and tones it Good while the window is open and the tap advances |
| 1528 | Lamp | lamp/indicator | RELOCATED | the lamp is the chip's tone (Good while the window is open and the tap advances): the Section row draws no dot (show-dot false in sectionsFor) |
| 1529 | Tooltip | tooltip | DEFERRED | the row's sentence is not found in tools_bank.slint or main.slint: main.slint passes no tooltip to the Section row |

## TOOLS bank — DEMOD SCOPE (window)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1530 | Window | window | PRESENT | DemodScopeWindow (ui/scope_modes.slint): a TornWindow 780 x 540 over the main window's centre, placed lower right inside it as the classic opens it, movable, resized by its grip, rolled up / maximised / closed from its chrome; an in-window window, not an operating-system one (the kit's torn-off pages are the same). Its Close key and a second press of the rail row shut it (scope-demod-open.txt) |
| 1531 | Input key AUDIO | key (latched, one of row) | PRESENT | LatchKey -> ScopeEvents.demod-signal(i) -> DemodScope::setSignal (scope_logic.cpp); drive: scope-demod-inputs.txt reads state scope.signal back |
| 1532 | Input key SPEC | key (latched) | PRESENT | LatchKey -> ScopeEvents.demod-signal(i) -> DemodScope::setSignal (scope_logic.cpp); drive: scope-demod-inputs.txt reads state scope.signal back |
| 1533 | Input key I/Q | key (latched) | PRESENT | LatchKey -> ScopeEvents.demod-signal(i) -> DemodScope::setSignal (scope_logic.cpp); drive: scope-demod-inputs.txt reads state scope.signal back |
| 1534 | Input key VECTOR | key (latched) | PRESENT | LatchKey -> ScopeEvents.demod-signal(i) -> DemodScope::setSignal (scope_logic.cpp); drive: scope-demod-inputs.txt reads state scope.signal back |
| 1535 | Input key MPX | key (latched) | PRESENT | LatchKey -> ScopeEvents.demod-signal(i) -> DemodScope::setSignal (scope_logic.cpp); drive: scope-demod-inputs.txt reads state scope.signal back; the key is ABSENT outside WFM (DemodScope::mpxAvailable), a saved MPX is shown as SPEC meanwhile and not rewritten |
| 1536 | TIME label | readout | PRESENT | Text TIME beside the time-base keys |
| 1537 | Time base - | button | PRESENT | LatchKey (accessible 'Shorter time base') -> demod-timebase(-1) -> DemodScope::stepTimebase: the ladder 1, 2, 5, 10, 20, 50 ms, clamped at its ends, refused and greyed on SPEC and MPX; scope-demod-inputs.txt |
| 1538 | Time base + | button | PRESENT | LatchKey (accessible 'Longer time base') -> demod-timebase(+1); scope-demod-inputs.txt reads state scope.timebase and the sweep (4800 -> 9600 samples at 20 ms/DIV) |
| 1539 | Time base readout | readout | PRESENT | Text from DemodReadouts::timebaseKey ('10 ms/DIV'; '-- (spectrum)' on a spectrum) |
| 1540 | GAIN label | readout | PRESENT | Text GAIN beside the attenuator keys |
| 1541 | Gain - | button | PRESENT | LatchKey (accessible 'Less gain') -> demod-gain(-1) -> DemodScope::stepGain: refused while AUTO is ticked and on a spectrum, clamped at the ladder's ends; scope-demod-inputs.txt |
| 1542 | Gain + | button | PRESENT | LatchKey (accessible 'More gain') -> demod-gain(+1); scope-demod-inputs.txt reads state scope.gainIndex and scope.gain |
| 1543 | Gain readout | readout | PRESENT | Text from DemodReadouts::gainKey ('100 mV/DIV', '10 dB/DIV' on a spectrum) |
| 1544 | AUTO | checkbox | PRESENT | Checkbox AUTO (the third control labelled AUTO: the strip's gain and squelch carry the first two) -> demod-auto -> DemodScope::setAutoGain; the ranging is one detent a tick from the pre-average peak (scope_logic.cpp); greyed on a spectrum; tooltip drawn |
| 1545 | DISPLAY label | readout | PRESENT | Text DISPLAY beside the display keys |
| 1546 | Display key NORM | key (latched) | PRESENT | LatchKey NORM -> demod-display(0) -> DemodScope::setDisplay; tooltip drawn; scope-demod-inputs.txt reads state scope.display |
| 1547 | Display key AVG | key (latched) | PRESENT | LatchKey AVG -> demod-display(1): ScopeMemory::average blends the feed (tau 0.5 s) before it is drawn; tooltip drawn |
| 1548 | Display key PERSIST | key (latched) | PRESENT | LatchKey PERSIST -> demod-display(2): the audio and I/Q envelopes and the spectrum's held peaks through ScopeMemory as in the classic; the VECTOR figure's memory is a decaying phosphor buffer, not 240 stroked ghost figures (docs 6.57: measured 500 ms a frame as ghosts, a departure) |
| 1549 | Scope face | overlay/marker | PRESENT | DemodTube: the bay, the bezel and the glass; the glass is paintDemodFace (scope_compositor.cpp) rasterised at the size the tube reports (demod-resized), two buffers alternated: the 10 x 8 graticule with its minor ticks, the triggered envelope trace, the spectrum line, I above and Q below the axis, the vector figure, the scan lines; a tube under 120 x 100 draws only the glass |
| 1550 | Face caption / legend | readout | PRESENT | GlassText along the foot of the glass: the signal's caption, the display word in amber when it is not NORM, the time base or '0 - 20.0 kHz' in the middle, the gain at the right, 'AUDIO FROM <plugin>' at the top left while a plugin owns the speakers |
| 1551 | Band labels on spectrum | overlay/marker | PRESENT | the multiplex's rules are painted (a bracket for a band, one rule for a tone) and its names are GlassText laid where they fit the room their band owns (DemodScope::mpxMarks over fm_mpx_plan.hpp); drawn under the beam |
| 1964 | Dead-tube sentences | readout | PRESENT | DemodReadouts::dead centred on the glass instead of a flat line at zero: NO SAMPLES - RECEIVER STOPPED, NO AUDIO IN THE TAP, NO MULTIPLEX IN THE TAP, NO BASEBAND IN THE TAP (test_slint_host_scope; scope-demod-open.txt reads state scope.dead) |
| 1965 | AUDIO FROM legend | readout | PRESENT | GlassText at the glass's top left while a plugin owns the speakers: 'AUDIO FROM <plugin>' (DemodReadouts::audioFrom, never on the baseband inputs) |

## TOOLS bank — Radar (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1552 | Section plate | key (collapsible) | RELOCATED | the Section row 'Radar' of bank 2 (main.cpp sectionsFor: chip '--', no dot; it starts folded, openSections[2], and a press folds it); the fixed chip word OFF with a dark lamp is tools_logic's (readTools chips[kRowRadar]), no tools publisher is in the tree |
| 1553 | No-position warning | readout | PRESENT | RadarBody amber Note (RadarModel.warning); readRadar sets it while FOXAPI_RX_POSITION_SET is clear |
| 1554 | Position offers | (see "Receiver position offers") | PRESENT | PositionOffersView inside RadarBody: the two offer keys, the hint, the typed entry and the GPS row while no position is set; with one set it leads with the known-position line (green) |
| 1555 | Receiver position fold | list (tree node) | RELOCATED | another form of the same control: no tree node, the PositionOffersView block is always open and, once a position is set, starts with the known-position line ('51.50000N 0.12000W', green) where the fold header was; there is no fold to open on a GPS fix |
| 1556 | Radar scope / Leave radar scope | button (full-width) | PRESENT | RadarBody full-width Key (lit while the scope shows; label Radar scope / Leave radar scope) -> radar-scope-clicked -> applyRadarScopeClicked ([local] Deferred 'radar-scope') -> ToolsPublisher::Hooks::radarScopeClicked -> ScopePublisher::toggleRadar: the SCOPE tab (view-index 2) opens, the view it came from is remembered; the second press, EXIT SCOPE and Esc go back (scope-radar-open.txt). DEPARTURE: the classic's scope replaces rail, centre and status column; here it fills the centre column, the rail and the status bar stay |
| 1557 | Radar scope tooltip | tooltip | PRESENT | the radar scope key's tooltip (the ImGui sentence through tr()) |
| 1558 | No-tracker note | readout | PRESENT | RadarBody Note scope-note: readRadar sets 'The scope will be empty. <why>' while the track-source census (ReadOptions::track_census) has no live source, naming no decoder / stopped / refused |

## TOOLS bank — Receiver position controls (shared: Radar fold, radar-scope empty state, map pages)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1559 | Traffic-centre offer | button (full-width) | PRESENT | OfferKey (a sentence that wraps) -> position-heard -> applyPositionHeard: FOXAPI_OP_SET_POSITION with the mean of the aircraft heard (ReadOptions::traffic_*), only with three or more heard and a pair the position rule accepts, then the coverage restarts; the label says '(n heard, need 3)' or '(none heard yet)' and the key is disabled until ready; tooltip drawn |
| 1560 | Map-centre offer | button (full-width) | PRESENT | OfferKey -> position-map -> applyPositionMap: SET_POSITION from a map page's centre (ReadOptions::map_*), nothing for an unacceptable centre; the key is not drawn while no map page offers one; tooltip drawn. The publisher must fill map_* from the kit's map page (no tools publisher is in the tree) |
| 1561 | Offers hint | readout | PRESENT | Note hint under the offers |
| 1562 | Latitude field | number field | PRESENT | LineInput (mono, right-aligned) with the tooltip: Enter or focus loss commits -> position-lat -> applyPositionLat, a [local] PositionLat draft; the text shown is ReadOptions::lat_text, five decimals (coordinateText) |
| 1563 | Longitude field | number field | PRESENT | LineInput beside it with the tooltip -> position-lon -> applyPositionLon, a [local] PositionLon draft |
| 1564 | Set RX here | button (small) | PRESENT | Key (set-label, 'Set RX here') flushes both fields then -> position-set -> applyPositionSet: SET_POSITION only when receiverPositionAcceptable (0,0 and out-of-range send nothing) then the coverage restarts; disabled with set-tooltip (the Gulf of Guinea sentence while untouched, the range sentence otherwise) and the red Note under it |
| 1565 | GPS hint | readout | PRESENT | Note gps-hint ('or read it from a GPS receiver on a serial port:'), drawn with the GPS row |
| 1566 | GPS port field | text field | RELOCATED | the SYSTEM family's GPS row (row 1442) drawn here by PositionOffersView from the same GpsRow model: the port is a DropKey over the scanned ports (SelectItem list), so the typed COM3 box is not drawn and the dropdown is not disabled while a read listens |
| 1567 | GPS port arrow | button (arrow) + popup list | RELOCATED | the same DropKey is the arrow and the list (SYSTEM row 1442; the scan is the Refresh key, row 1439); the 'no serial ports found' entry is not found in tools_bank.slint |
| 1568 | GPS baud | dropdown/combo | RELOCATED | the baud DropKey (84 px) of the SYSTEM family's GPS row (row 1442) -> SystemEvents.gps-baud (applyGpsBaud: save gpsBaud) |
| 1569 | Read position from GPS | button | RELOCATED | the read key of the SYSTEM family's GPS row (row 1442): Key (key-label, key-enabled) -> SystemEvents.gps-toggle (applyGpsToggle: FOXAPI_OP_GPS start with the port and baud); disabled with no port named |
| 1570 | Stop (GPS) | button | RELOCATED | the same Key lit as Stop while listening (SYSTEM row 1442) -> gps-toggle stops the read |
| 1571 | GPS status line | readout | RELOCATED | Note gps status in its tone, the SYSTEM family's GpsRow.status (row 1442), plus the satellites line under it |
| 1572 | GPS refusal line | readout | RELOCATED | the amber Note GpsRow.refusal (SYSTEM row 1442) |
| 1573 | Serial-ports footnote | readout | PRESENT | Note footnote 'All serial-port settings are also under SYSTEM > Serial ports.' at the foot of the block |

## TOOLS bank — Radar scope (full-window mode)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1574 | EXIT SCOPE | button | PRESENT | Key EXIT SCOPE, drawn first in the bar -> radar-exit -> ScopePublisher::leaveRadar (the view the scope was entered from); scope-radar-open.txt |
| 1575 | Escape key | key (keyboard) | PRESENT | the root FocusScope's 'escape' shortcut -> leaveRadar while the SCOPE tab shows and nothing is edited (main.cpp); main.slint's `changed view-index` hands the keyboard to the root when the tab opens, so a field that held it does not swallow the key; scope-radar-open.txt |
| 1576 | RANGE caption | readout | PRESENT | Text RANGE in the bar |
| 1577 | Range - | button | PRESENT | Key (accessible 'Range down') -> radar-range(-1) -> RadarScope::stepRange: one rung of the ladder 10, 25, 50, 100, 200, 400, 800, 1600, never wrapping, disabled at its end (RadarFrame::canStepDown); scope-radar-open.txt reads state scope.range |
| 1578 | Range readout | readout | PRESENT | Text from RadarFrame::rangeText ('200 NM'), amber |
| 1579 | Range + | button | PRESENT | Key (accessible 'Range up') -> radar-range(+1); scope-radar-open.txt |
| 1580 | Wheel hint | readout | PRESENT | Text 'the mouse wheel over the scope steps it too' |
| 1581 | Empty state | readout | PRESENT | RadarScopeView (ui/scope_modes.slint) with no receiver position: the sentence, the paragraph and PositionOffersView (the same block as the Radar fold and the map pages, from the TOOLS bank's RadarModel.offers) instead of the cabinet; scope-radar-open.txt shot scope-radar-empty.png |
| 1582 | SIG gauge | overlay/marker (gauge) | PRESENT | Gauge SIG: 18 segments lit from the bottom (RadarFrame::sigLit), '--' when the receiver is stopped or the level is not finite |
| 1583 | ALT gauge | overlay/marker (gauge) | PRESENT | Gauge ALT: 18 segments, 'FL350' / 'NO ALT' / '--' for the selected aircraft |
| 1584 | Maker plate | overlay/marker | PRESENT | the deck's maker plate: the fox mark, FOX and '& SCHIRMYVER INDUSTRIES' (engraved, not translated), without the classic's green diamond |
| 1585 | Range drums | readout | PRESENT | Drum (a bay with its caption and four digits): 'TGT RANGE NM' with the selected aircraft's range, else 'SET RANGE NM' with the scope's (RadarFrame::drumCaption / drumRange); digits in a bay, not rolling cells |
| 1586 | Track drums | readout | PRESENT | Drum TRACKS: three digits, every aircraft heard, ungated (RadarFrame::drumTracks); scope-radar-targets.txt reads state scope.drumTracks |
| 1587 | GAIN knob | dial/knob | PRESENT | GainKnob: a drag of 12 px or a wheel notch is one detent -> radar-knob -> RadarScope::knobDetent (2 units a detent, clamped to the stage's travel, kept 0.5 s locally through ScopeGainLocal) -> FOXAPP_OP_SET_GAIN_NO_READBACK with the stage and the dB; dead without a radio or with its AGC on (state scope.knobDead). The strip's Gain lane stays a gain control too |
| 1588 | Knob scale ticks | readout | DEFERRED | the five tick numbers around the knob are in the model (RadarFrame::knobTicks, knobLit) but the view does not draw them: the pointer, the figure under the knob and the stage note are |
| 1589 | Knob stage note | readout | PRESENT | Text under the knob: the stage's name, 'no device' or 'auto gain is on' (RadarFrame::knobNote); the figure above it ('30 dB', 'N/A', 'AUTO') |
| 1590 | POWER button | button (round, lit ring) | PRESENT | Key (accessible 'POWER'), lit while the receiver runs -> radar-power -> FOXAPI_OP_RUN with ival[0] = 0 or 1; scope-radar-open.txt stops and starts the receiver and reads state running. A lit key, not the classic's round ring |
| 1591 | Power state text | lamp/indicator | PRESENT | the key's own word: ON LINE / STANDBY (RadarFrame::powerText; state scope.powerText) |
| 1592 | Tube (canvas) wheel | drag/gesture | PRESENT | RadarTube TouchArea scroll-event -> radar-wheel(+1 wheel up) -> RadarScope::wheel: wheel up = range down one rung (the classic's rule); test_slint_scope_modes drives it with real scroll events, test_slint_host_radar pins the sign |
| 1593 | Tube drag | drag/gesture | PRESENT | RadarTube TouchArea: a press that moves over 2 px reports its travel (radar-drag) -> RadarScope::dragBy pans the VIEW centre (scopeViewMoved), the range stays anchored to it; the release says it was a drag (radar-released) and that click is swallowed |
| 1594 | Tube double-click | drag/gesture | PRESENT | RadarTube TouchArea double-clicked -> radar-double-clicked -> RadarScope::doubleClick (resetPan) |
| 1595 | Tube click on aircraft | drag/gesture | PRESENT | a plain click -> radar-released(x, y, false) -> RadarScope::releasePointer selects the nearest aircraft within its marker radius; state scope.selected. Hit-tested in the host from the projected positions (one touch area, not one per target) |
| 1596 | Tube click on empty | drag/gesture | PRESENT | a plain click on empty glass clears the selection and recentres the view on the click (RadarScope::releasePointer) |
| 1597 | Range rings + labels | overlay/marker | PRESENT | paintRadarGround strokes the four rings (the outer one brighter and wider) and RadarFrame::ringLabels are Text at their 12 o'clock places ('%.4g NM'); a panned view keeps the rings on the tube's centre, as the classic does |
| 1598 | Bearing ticks | overlay/marker | PRESENT | paintRadarGround strokes the twelve bearing ticks (cardinals longer); RadarFrame::bearingLabels place N, E, S, W and, if the tube is big enough, the three-digit bearings |
| 1599 | MODE ADS-B / position | overlay/marker | PRESENT | Text in the tube's corners: '%d PLOTTED' (what is drawn), the range, 'MODE ADS-B' and the receiver's position with hemisphere letters |
| 1600 | Aircraft icons and trails | overlay/marker | PRESENT | targets as items over the ground: the Slint map's aircraft glyph (icons/map-aircraft.svg) turned to its course in its altitude band's colour (grey-blue when unknown, red for an emergency, faded by age), a ring when selected or in emergency, its label beside it; trails stroked into the ground (off / line / ribbon, 256 points, a point when it moved 50 m). DEPARTURE: one glyph, not the classic's silhouettes by category and registry type |
| 1601 | Antenna mark | overlay/marker | PRESENT | the view centre's pink dot, and while the view is panned a green cross and 'RX' at the antenna's place (RadarFrame::rxMarkX / rxMarkY) |
| 1966 | Tube optics: ground, coastline, phosphor, sweep | overlay/marker | PRESENT | paintRadarGround (radar_compositor.cpp): the dark ground, the PHOSPHOR green wash with the compiled-in coastline under it (azimuthal-equidistant projection, in range only), the crosshair, the scan lines; the sweep is one fan picture (paintSweepFan) turned about the view centre by the host's angle, 4.2 s a turn. With PHOSPHOR off the ground is plain and the coast is not drawn (the classic still draws it) |
| 1967 | Basemap tiles under the tube and the attribution line | overlay/marker | DEFERRED | the classic reprojects a basemap plugin's tiles under the glass (scopeGroundPoint / scopeTileWindow) and prints its attribution under the instrument; the Slint tube draws the coastline only. The kit's BasemapClient is in the host; a ScopeGroundCompositor over it is the work |
| 1968 | Cabinet case: gradient, screws, layout rules | overlay/marker | DEFERRED | the classic's brass case, its four screws and its scale rule (822 px slab, 0.42-2.2) are not drawn: the cabinet is Workbench panels sized from the centre column (the tube is the largest square the height and width allow; the LCD is dropped under 900 px of width) |

## TOOLS bank — Radar data panel (LCD) - three tabs

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1602 | Tab HOME | button (tab) | PRESENT | Key HOME -> radar-lcd-tab(0) -> RadarScope::setLcdScreen; scope-radar-open.txt reads state scope.lcd |
| 1603 | Tab FLIGHT | button (tab) | PRESENT | Key FLIGHT -> radar-lcd-tab(1); the default screen, as in the classic |
| 1604 | Tab SYS | button (tab) | PRESENT | Key SYS -> radar-lcd-tab(2) |
| 1605 | CONTACTS PLOTTED | readout | PRESENT | RadarLcd HOME: 'CONTACTS PLOTTED', the big count and 'of %d tracked   %d NM%s' |
| 1606 | Contact rows | list | PRESENT | RadarLcd HOME list: 'ALL CONTACTS', nearest first, two lines each (name and altitude in its band's colour; range and bearing), a Flickable; a click -> radar-contact(id) -> RadarScope::selectContact selects and shows FLIGHT; scope-radar-targets.txt clicks the row by its name |
| 1607 | Empty-state text | readout | PRESENT | RadarLcd HOME with no contact: NOTHING CURRENT with the aged paragraph, or NOTHING BEING HEARD with its paragraph |
| 1608 | Option row PHOSPHOR | toggle | PRESENT | RadarLcd SYS row PHOSPHOR (ON / OFF) -> radar-option(0) -> RadarScope::cycleOption; scope-radar-open.txt reads state scope.phosphor |
| 1609 | Option row SWEEP | toggle | PRESENT | RadarLcd SYS row SWEEP (ON / OFF) -> radar-option(1); state scope.sweep |
| 1610 | Option row TRAILS | toggle (cycles 3) | PRESENT | RadarLcd SYS row TRAILS (OFF / LINE / RIBBON) -> radar-option(2); state scope.trail |
| 1611 | Option row FILTER | toggle (cycles 3) | PRESENT | RadarLcd SYS row FILTER (ALL / ALERT / NAMED) -> radar-option(3); scope-radar-targets.txt reads state scope.filter and scope.plotted |
| 1612 | Facts | readout | PRESENT | RadarLcd SYS: RANGE and ANTENNA facts (RadarFrame::sysRange / sysAntenna) |
| 1613 | Help paragraph | readout | PRESENT | RadarLcd SYS help paragraph (RadarFrame::sysHelp) |
| 1614 | Detail lines | table | PRESENT | RadarLcd FLIGHT: FLIGHT, OPERATOR, TYPE, REG, STATUS, ALTITUDE, SPEED, HEADING, RANGE, BEARING from buildScopeDetailLines (dim when unknown, red for an emergency); scope-radar-targets.txt reads state scope.flight |
| 1615 | Nothing-selected text | readout | PRESENT | RadarLcd FLIGHT with nothing selected: NOTHING SELECTED and its hint; TARGET LOST when the selected aircraft is no longer plotted |
| 1616 | Unavailable note | readout | PRESENT | RadarLcd FLIGHT foot: scopeUnavailableNote ('No squawk, route or airline mark: the track interface does not carry them.') |

## TOOLS bank — Bookmarks (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1617 | Section plate | key (collapsible) | RELOCATED | the Section row 'Bookmarks' of bank 2: main.cpp sectionsFor draws the live count as its chip and no lamp; tools_logic's chip and lamp (lit when the count is above 0) are not published (no tools publisher is in the tree) |
| 1618 | Name field | text field | PRESENT | LineInput 'name' (placeholder) -> bookmark-name on commit -> applyBookmarkName, a [local] BookmarkName draft |
| 1619 | Add current | button | PRESENT | Key (add-label, 'Add current') flushes the name and group boxes -> bookmark-add -> applyBookmarkAdd: FOXAPI_OP_BOOKMARK_ADD with the name, or FOXAPP_OP_BOOKMARK_GROUP (action 0, group + 0x1F + name) when the group box holds a name; a blank name is named by its frequency in the engine (addBookmarkHere) |
| 1620 | Group field | text field | PRESENT | LineInput 'group (optional)' with the tooltip -> bookmark-group on commit -> [local] BookmarkGroup |
| 1621 | Group pick arrow | dropdown/combo (no preview) | PRESENT | a 28 px DropKey of the groups the list holds, drawn only when there are any -> bookmark-group(<group name>): the pick types the name into the group box |
| 1622 | Import path | text field | PRESENT | LineInput with the placeholder -> bookmark-import-path on commit -> [local] BookmarkImportPath |
| 1623 | Import | button | PRESENT | Key (import-label) with the tooltip, disabled while an import runs (import-busy) -> bookmark-import -> applyBookmarkImport: an empty path opens the import dialog ([local] OpenImportDialog, the SIGNAL family's, rows 1380-1386); a path with a root = FOXAPP_OP_BOOKMARK_IMPORT_FILE; any other path = [local] ImportLookup for the publisher's worker |
| 1624 | Import note | readout | PRESENT | Note import-note (fact.status.bookmarkNote: the result of an import, rename or removal) |
| 1625 | Disk wait line | readout | PRESENT | Note disk-wait ('Waiting for the disk to open the file: n s'), drawn from one second of a read |
| 1626 | Search box | text field | PRESENT | LineInput 'search names, groups or MHz' -> bookmark-search on every edit -> [local] BookmarkSearch, so the list filters as it is typed |
| 1627 | Group filter | dropdown/combo | PRESENT | DropKey (104 px) 'Every group' plus each group, drawn only when the list holds a group -> bookmark-filter(i) -> applyBookmarkFilter: [local] BookmarkFilter by the group's name |
| 1628 | Favourites only | checkbox | PRESENT | Tick -> bookmark-favourites -> [local] BookmarkFavourites |
| 1629 | On the spectrum | checkbox | PRESENT | Tick with the tooltip -> bookmark-on-spectrum -> [local] BookmarkOnSpectrum, saved as bookmarkMarkers through ToolsPublisher::Hooks::saveBookmarkOptions and read back at start (docs 6.52); main.cpp publishes no bookmark marks while it is off |
| 1630 | Stack close names | checkbox | PRESENT | Tick with the tooltip, disabled while On the spectrum is off (stack-enabled) -> bookmark-stack -> [local] BookmarkStack, saved as bookmarkStackNames (docs 6.52); main.cpp stacks clashing names onto up to three rows by host_logic bookmarkNameRows (the classic rule of gui/bookmark_marker_geometry.hpp), one row with it off |
| 1631 | Count line | readout | PRESENT | Note count-line ('n shown of m') |
| 1632 | Export for SDR# | button (small) | PRESENT | xs Key in a KeyFlow (wraps under the next keys in a narrow column), label empty (key not drawn) for an empty view, disabled while an export is written -> bookmark-export -> applyBookmarkExport: FOXAPP_OP_BOOKMARK_EXPORT with the ids of the rows shown |
| 1633 | Remove this group | button (small) | PRESENT | xs Key, drawn only with a group chosen -> bookmark-remove-group -> applyBookmarkRemoveGroup: FOXAPI_OP_BOOKMARK_REMOVE_GROUP and the filter back to every group; no confirmation, as in the ImGui window |
| 1634 | Rename this group | button (small) | PRESENT | xs Key (lit while open) that shows / hides BookmarksBody's rename field and tells the host (bookmark-rename-open -> applyBookmarkRenameOpen): the draft is seeded with the group's own name every time it opens, so a name typed and abandoned does not come back (docs 6.61; drive tools-bookmarks-rename-key.txt, RED first: 'it reads Flying') |
| 1635 | Rename field | text field | PRESENT | LineInput 'new group name', drawn while renaming -> bookmark-rename-draft on commit; Enter -> bookmark-rename (applyBookmarkRename) and the field closes |
| 1636 | Rename | button | PRESENT | Key 'Rename' beside the field: flushes it -> bookmark-rename -> applyBookmarkRename: FOXAPP_OP_BOOKMARK_GROUP action 2 (from + 0x1F + to; merges when the name exists), nothing for an empty or unchanged name; the filter follows the group |
| 1637 | Row scan checkbox | checkbox | PRESENT | Tick (14 px) with the tooltip on each row -> bookmark-tick(id, on) -> FOXAPP_OP_BOOKMARK_TICK |
| 1638 | Row favourite star | button | PRESENT | StarKey ('*' amber while a favourite, '.' dim) -> bookmark-favourite(id, on) -> FOXAPI_OP_BOOKMARK_FAVOURITE |
| 1639 | Row | list (selectable) | PRESENT | BookmarkRowView's selectable 'name  145.6000 MHz  [group]' (mono, elided; hover highlight) -> bookmark-tune(id) -> FOXAPI_OP_BOOKMARK_TUNE; the rows scroll at 12 (ScrollRows). The inventory lists no hover tooltip for the row and the ImGui row shows none, so there is nothing to defer |
| 1640 | Row context menu | context menu | PRESENT | a right-click on the row (pointer-event, right button up) opens a PopupWindow beside it: the row's name (BookmarkRow.name) and 'Move to group' -> bookmark-menu(id) -> [local] BookmarkMenu. The menu's closed state is not reported (a PopupWindow is modal and Slint 1.18 has no closed callback), so BookmarksModel.menu-row is never drawn from and returns to -1 only through a pick (applyBookmarkMove) |
| 1641 | Menu: No group | menu item | PRESENT | DropRow 'No group' (ticked while the row has none) -> bookmark-move(id, '') -> FOXAPP_OP_BOOKMARK_GROUP action 1; nothing when the row is already ungrouped; the menu closes |
| 1642 | Menu: group list | list | PRESENT | DropRows of the groups (plain up to eight, a scrolling Flickable beyond) -> bookmark-move(id, group) |
| 1643 | Menu: new group | text field | PRESENT | LineInput 'new group name' at the foot of the menu: Enter with text -> bookmark-move(id, text) |
| 1644 | Row delete | button | PRESENT | xs icon Key 'Delete this bookmark' -> bookmark-delete(id) -> FOXAPI_OP_BOOKMARK_REMOVE, at once |
| 1645 | Error line | readout | PRESENT | red Note error (fact.status.bookmarkError) |
| 1646 | Spectrum markers | overlay/marker | RELOCATED | the spectrum's own bookmark ticks and names (SpectrumView, main.cpp bookmarks marks): phase-1 rows 259 and 260; the strip's two settings are On the spectrum and Stack close names above |
| 1647 | Drop-a-file import | drag/gesture | DEFERRED | not found in tools_bank.slint, main.slint or main.cpp: no file-drop event is wired into the Slint window (the same gap as row 1383); the path box and the import dialog are the ways in |

## TOOLS bank — Import a frequency list (dialog)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1648 | Dialog | dialog | RELOCATED | the SIGNAL family's ImportDialog (row 1380), shared with this bank: opened by an empty Import path |
| 1649 | Reason line | readout | RELOCATED | the SIGNAL family's ImportDialog reason line (row 1381) |
| 1650 | Preset line | readout | PRESENT | the dialog's preset line (row 1382): the Airband Import over an empty box opens the dialog for the preset, and its rows join it (docs 6.61) |
| 1651 | Drop zone | overlay/marker | DEFERRED | the drop zone is words only in the SIGNAL family's dialog and no file-drop event is wired (row 1383) |
| 1652 | or / Browse... | button | DEFERRED | the SIGNAL family's dialog has a path field in place of the system file chooser (row 1384) |
| 1653 | Waiting line | readout | DEFERRED | the SIGNAL family's dialog has no reading line (row 1385) |
| 1654 | Cancel | button | RELOCATED | the SIGNAL family's Cancel key (row 1386) |
| 1655 | Escape | key (keyboard) | PRESENT | Esc closes the dialog: key_dispatch escapeStep takes the import dialog first, then the store's plugin page, else the overlays; the sheet's own key scope covers a focused field (docs 6.61; drive escape-leaves-dialog.txt) |

## TOOLS bank — Scanner (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1656 | Section plate | key (collapsible) | RELOCATED | the Section row 'Scanner' of bank 2: main.cpp sectionsFor draws SCANNING (green) or OFF from FOXAPI_RX_SCANNER_ACTIVE and no lamp; the SCAN / IDLE words and the lit lamp are tools_logic's (readScanner chip) and not published (no tools publisher is in the tree) |
| 1657 | Ticked frequencies | checkbox | PRESENT | Tick (label 'Ticked frequencies (n)') with the tooltip -> scanner-ticked -> applyScannerTicked: FOXAPP_OP_SCANNER_LIST (1 list mode, 0 range mode) |
| 1658 | Start MHz | number field | PRESENT | LabelledField (caption above, mono, right-aligned), greyed in ticked mode -> scanner-start on commit -> applyScannerField: [local] ScannerDraft and FOXAPP_OP_SCANNER_RANGE for its bit (reconfigure bit while a scan runs); text that is no number sends nothing; four decimals |
| 1659 | Stop MHz | number field | PRESENT | LabelledField, greyed in ticked mode -> scanner-stop -> applyScannerField (range bit 2) |
| 1660 | Step kHz | number field | PRESENT | LabelledField, greyed in ticked mode -> scanner-step -> applyScannerField (range bit 4) |
| 1661 | Dwell ms | number field | PRESENT | LabelledField -> scanner-dwell on commit -> applyScannerField: FOXAPI_OP_SCANNER_CONFIG with the four timings |
| 1662 | Hold ms | number field | PRESENT | LabelledField -> scanner-hold -> FOXAPI_OP_SCANNER_CONFIG |
| 1663 | Resume ms | number field | PRESENT | LabelledField -> scanner-resume -> FOXAPI_OP_SCANNER_CONFIG |
| 1664 | Listen ms | number field | PRESENT | LabelledField with the tooltip -> scanner-listen -> FOXAPI_OP_SCANNER_CONFIG |
| 1665 | Start scan | button (full-width) | PRESENT | full-width Key (start-label) that flushes the seven fields first, disabled in ticked mode with nothing ticked (start-disabled) -> scanner-run -> applyScannerRun: FOXAPP_OP_SCANNER_LIST action 2 in list mode (the engine stands the Airband monitor down), else FOXAPI_OP_SCANNER_RUN with the stored range |
| 1666 | Stop scan | button (half) | PRESENT | half-width Key, lit, drawn only while scanning -> scanner-halt -> FOXAPI_OP_SCANNER_RUN 0 |
| 1667 | Skip | button | PRESENT | half-width Key beside Stop scan, drawn only while scanning -> scanner-skip -> FOXAPI_OP_SCANNER_SKIP |
| 1668 | Status line | readout | PRESENT | Note status ('Scanning / 145.5000 MHz') in its tone: Idle neutral, Scanning and Paused good, Holding amber (scannerStatusLine) |
| 1669 | User retune | drag/gesture | PRESENT | not a Slint control: the engine stops the scan when the readback leaves the last commanded frequency (engine.cpp user-tune check against scannerExpectedAbsHz_), so any tune from this window stops it |

## TOOLS bank — Airband (rail section)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1670 | Section plate | key (collapsible) | RELOCATED | the Section row 'Airband' of bank 2: main.cpp sectionsFor draws a static '--' chip and no lamp; the LISTEN / IDLE words and the lit lamp are tools_logic's (readAirband chip) and not published (no tools publisher is in the tree) |
| 1671 | Airport code | text field | PRESENT | LineInput with the placeholder -> airband-code on commit -> [local] AirbandCode (upper-cased by applyAirbandCode); Enter -> airband-lookup |
| 1672 | Look up | button | PRESENT | Key (lookup-label) with the tooltip, disabled while the code box is empty -> airband-lookup -> applyAirbandLookup: one airport = FOXAPP_OP_BOOKMARK_ADD_ROWS (its rows, ask AirportRows); none = the 'No airport with the code' note; several = the choices and the 'pick one' note |
| 1673 | Nearest airports | button (small) | PRESENT | Key in a KeyFlow, disabled until the receiver has a position (nearest-disabled) -> airband-nearest -> applyAirbandNearest: the six nearest airports; the two tooltip sentences are not found in tools_bank.slint (a KeyFlow key carries no tooltip) |
| 1674 | Hide | button (small) | PRESENT | Key beside it, drawn while the nearest list is shown -> airband-hide -> clear_nearest |
| 1675 | Nearest list | list | PRESENT | AirportLine rows ('EGLL  London Heathrow  23 km') -> airband-airport(i) -> applyAirbandAirport: adds that airport's rows and closes the list |
| 1676 | Lookup choices | list | PRESENT | AirportLine rows ('ORD  Chicago O'Hare  (United States)') -> airband-ambiguous(i) -> applyAirbandAirport (choice 1000 + i) |
| 1677 | Preset label | readout (tooltip) | PRESENT | RowLabel 'Preset'; the preset help sentence is the tooltip of the name field beside it (preset-help), not of the label |
| 1678 | Preset name | text field | PRESENT | LineInput 'Preset' with the tooltip and the 28 px arrow listing the presets that hold a playable row -> airband-preset-name on commit / airband-preset(i) -> [local] AirbandPresetName |
| 1679 | Frequency (MHz) | text field | PRESENT | mono LineInput 'Frequency (MHz)' -> airband-freq on commit -> [local] AirbandFreq |
| 1680 | Mode | dropdown/combo | PRESENT | DropKey AM / NFM (76 px) -> airband-mode(i) -> [local] AirbandMode |
| 1681 | Name | text field | PRESENT | LineInput 'name' -> airband-name on commit -> [local] AirbandName |
| 1682 | Add | button | PRESENT | Key with the tooltip, disabled with no frequency -> airband-add -> applyAirbandAdd: FOXAPP_OP_BOOKMARK_ADD_ROWS with one ticked AM or NFM row in the preset (ask TypedRow); a frequency parseMhz refuses gives the 'could not read frequency' note |
| 1683 | Note | readout | PRESENT | Note note (AirbandModel.note: outcomes and refusals) |
| 1684 | Import path | text field | PRESENT | LineInput 'SDR# frequencies.xml or .csv - its path' -> airband-import-path on commit -> [local] AirbandImportPath |
| 1685 | Import | button | PRESENT | Key, disabled while a read is out (import-busy) -> airband-import -> applyAirbandImport: a path = [local] PresetImportLookup (the publisher's worker sends the rows as ADD_ROWS chunks); an empty box = [local] OpenImportDialog for the preset, whose dialog names it, and a path typed there is applyAirbandImportInto (docs 6.61; drive tools-airband-import.txt imports the fixture airband-import.csv and reads three rows in the group Towers, none in the file's own group) |
| 1686 | Export CSV | button | PRESENT | Key in a KeyFlow, disabled with no rows or while an export is written -> airband-export -> applyAirbandExport: FOXAPP_OP_BOOKMARK_EXPORT action 1 with the preset (ask ExportPreset) |
| 1687 | Remove preset | button | PRESENT | Key beside it, disabled with no rows -> airband-remove-preset -> applyAirbandRemovePreset: FOXAPI_OP_BOOKMARK_REMOVE_GROUP with the preset (ask RemoveGroup); whether the engine stops LISTEN on it is not read here |
| 1688 | Preset note | readout | PRESENT | Note preset-note |
| 1689 | Open the folder | button (small) | PRESENT | Key in a KeyFlow, drawn after an export that wrote its file (folder-label) -> airband-folder -> [local] OpenFolder with fact.recorder.dir |
| 1690 | Group filter | dropdown/combo | PRESENT | DropKey (popup 250 px) 'Every ticked AM or NFM frequency' plus each playable group -> airband-group(i) -> applyAirbandGroup: [local] AirbandGroup, the preset field takes the group's name, and FOXAPP_OP_AIRBAND action 1 when the group changes while the monitor listens |
| 1691 | Count | readout | PRESENT | Note count-line ('n ticked of m') |
| 1692 | Tick all / Untick all | button (small) x2 | PRESENT | two keys in a KeyFlow, drawn only with a group on show and rows listed -> airband-tick-all / airband-untick-all -> applyAirbandTickAll: [local] TickRun of the rows that differ |
| 1693 | Busiest first | checkbox | PRESENT | Tick with the tooltip -> airband-busiest -> [local] AirbandBusiest (the read side sorts the rows by heard time) |
| 1694 | Status block | readout | PRESENT | Note hearing ('block on the air' and 'Hearing: x' joined by a newline; the pending sentence while waiting for the patch) |
| 1695 | Row tick | checkbox | PRESENT | Tick (14 px) with the tooltip on each channel row -> airband-tick(id, on) -> FOXAPP_OP_BOOKMARK_TICK |
| 1696 | Row lamp | lamp/indicator | PRESENT | LampDot green on AirbandRow.heard, off otherwise |
| 1697 | Row | list (selectable) | PRESENT | AirbandRowView's selectable '118.1000  name' (dim while listening and off the air) with the amber heard time -> airband-tune(id) -> FOXAPP_OP_AIRBAND action 6: the monitor stops and the receiver tunes there alone. The level tooltip is the next row |
| 1698 | Row level tooltip | tooltip | DEFERRED | the tooltip half of the inventory's one Row line 1318, ticked apart so the click is not marked missing: AirbandRowView draws no tooltip and AirbandRow has no tooltip field (readAirband makes the 'Level -42 dB (squelch -55 dB)' text as a host extra, AirbandRow.tooltip); the family did not build it |
| 1699 | Squelch | slider | PRESENT | SliderRow (squelch-text readout, tooltip) -> airband-squelch(norm) -> applyAirbandSquelch: FOXAPP_OP_AIRBAND action 4 with -110 + norm * 110 dB. The ImGui control sends on release; the Slint Slider calls changed on every drag step and has no release callback, so a drag sends one op per step unless the publisher (no tools publisher is in the tree) holds them back (unverified) |
| 1700 | Above the noise | button (small) | PRESENT | Key, disabled unless listening -> airband-above-noise -> FOXAPP_OP_AIRBAND action 3 (the engine's rule); no tooltip is drawn |
| 1701 | Hold | number field | PRESENT | LineInput (64 px, mono, right-aligned) 'hold 2.0 s' with the tooltip and a 's' unit label -> airband-hold on commit -> applyAirbandHold: FOXAPP_OP_AIRBAND action 5 with seconds clamped 0..60 (the first number in the text) |
| 1702 | LISTEN | button (full-width) | PRESENT | full-width Key LISTEN with the tooltip -> airband-listen -> applyAirbandListen: FOXAPP_OP_AIRBAND action 1 with the group on show (ask Listen); the hold box is flushed first |
| 1703 | STOP | button | PRESENT | the same key becomes the danger Key STOP while listening or waiting -> airband-listen -> FOXAPP_OP_AIRBAND action 0 |
| 1704 | Next block | button | PRESENT | Key beside STOP, drawn only when next-label is set (more than one block) -> airband-next -> FOXAPP_OP_AIRBAND action 2 |

## TOOLS bank — Map page (window per track-source plugin)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1705 | Window | window | RELOCATED | kit map page (6.38): a docked tab or a torn window titled '<plugin> map' (mapTitle), opened by the DECODE bank's map row (row 318); title, close and tear-off are rows 323-329 |
| 1706 | Fit | button (small) | RELOCATED | kit map page (6.38) row 357: the Fit key of MapToolbar (page-map-fit: centres and zooms on every visible target) |
| 1707 | Receiver position entry | number field x2 + button | RELOCATED | kit map page (6.38) row 358: Set RX here arms the next click on the map (FOXAPI_OP_SET_POSITION) in place of two typed boxes, and an RX readout shows the position; the typed latitude / longitude entry is the Radar fold's (rows 1562 and 1563), not repeated on the page |
| 1708 | Count line | readout | RELOCATED | kit map page (6.38) row 359: the count text ('n targets', 'n of m targets, k heard without a position') |
| 1709 | On screen only | checkbox | RELOCATED | kit map page (6.38) row 360: the On screen only checkbox |
| 1710 | Coverage | checkbox | RELOCATED | kit map page (6.38) row 361: the Coverage checkbox |
| 1711 | Trails | checkbox | RELOCATED | kit map page (6.38) row 362: the Trails checkbox |
| 1712 | Altitude colours | checkbox | RELOCATED | kit map page (6.38) row 363: the Altitude colours checkbox, disabled without Trails |
| 1713 | Trail style | dropdown/combo | DEFERRED | the saved style is applied by the map (kit map page row 364), but no trail-style dropdown is drawn on the page or in the Display body; not found in pages.slint |
| 1714 | Trail width | slider | DEFERRED | the saved width is applied by the map (kit map page row 365); no trail width slider is drawn on the page; the same setting is the SYSTEM Display body's Trail width (row 1398) |
| 1715 | Reset coverage | button (small) | RELOCATED | kit map page (6.38) row 366: the Reset coverage key |
| 1716 | Coverage readout | readout | RELOCATED | kit map page (6.38) row 367: the coverage status sentence |
| 1717 | GPS row | (shared GPS block) | RELOCATED | the kit map page draws no GPS row (6.38 row 368); the row lives in the SYSTEM bank's Serial ports (row 1442) and in the Radar fold (rows 1566 to 1572) |
| 1718 | Attribution credit | readout | RELOCATED | kit map page (6.38): MapPane's attribution line (the basemap's text, or 'Coastline: Natural Earth (public domain)' on the coastline map); row 1133 is the basemap plugin's own row |
| 1719 | Stop following? | dialog | DEFERRED | kit map page row 372 stays deferred: pages.slint has no Stop following? dialog and page-map-panned silently refuses a drag while following; Stop following is the key |
| 1720 | Stop following (dialog) | button | DEFERRED | no dialog, so no key (row 372); the toolbar's Stop following key is row 371 |
| 1721 | Keep following | button | DEFERRED | no dialog, so no Keep following key (row 372); the drag is refused instead |

## TOOLS bank — Track list (left of the map page)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1722 | Empty text | readout | RELOCATED | kit map page (6.38) row 369: the list's empty text 'No targets. Decoded aircraft, ships and stations appear here.'; the 'No targets on screen.' variant is not found in pages.slint |
| 1723 | Count | readout | RELOCATED | kit map page (6.38) row 359: the count text sits in the toolbar, not over the list |
| 1724 | Following note | readout | RELOCATED | kit map page (6.38) row 371: 'following <id>' and the Stop following key sit in the toolbar, not in the list pane |
| 1725 | Sort | dropdown/combo | RELOCATED | kit map page (6.38) row 369: a Sort key whose label is the sort key's name; each press steps to the next of the eight keys (a stepper in place of the dropdown), tooltip 'Order the list. Distance and bearing are measured from the RX position.' |
| 1726 | Sort direction | button (arrow) | RELOCATED | kit map page (6.38) row 369: an Up / Down key with the two 'click to reverse' tooltips |
| 1727 | Table | table | RELOCATED | kit map page (6.38) row 369: a Table with the columns Callsign, ID and Note; the Details / Info column is not drawn (row 370) |
| 1728 | Row (callsign cell) | list (selectable, spans row) | RELOCATED | kit map page (6.38) row 369: a click selects and centres the map on it (zoom unchanged), a double-click follows; list rows are not dimmed by staleness (only the map target's alpha follows it), and an estimated position carries the 'estimated' note in the Note column |
| 1729 | Details | button (small) | RELOCATED | kit map page (6.38) row 370: there is no Details key; a click on a row selects it for the Target details drawer |
| 1730 | Heard-without-position rows | list | RELOCATED | kit map page (6.38): muted, non-pressable rows with the target's id and its unplaced note, always listed (On screen only does not hide them) and with no Details key |
| 1731 | Distance footnote | readout | DEFERRED | not found in pages.slint or kit_publish.cpp: no footnote under the list; the RX readout says 'no receiver position: Set RX here, then click the map' instead |
| 1732 | Hover block | tooltip | RELOCATED | kit map page (6.38) row 369: the hover tip over a map target shows callsign, altitude (ft) and speed (kt) only, not the full detail block |

## TOOLS bank — Map canvas (all map pages)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1733 | Canvas pan | drag/gesture | RELOCATED | kit map page (6.38): MapPane panned -> page-map-panned moves the centre; while following the drag is refused without the dialog (rows 1719 to 1721) |
| 1734 | Canvas wheel | drag/gesture | RELOCATED | kit map page (6.38): MapPane zoomed -> page-map-zoomed, span x 0.85 per notch about the pointer |
| 1735 | Click target | drag/gesture | RELOCATED | kit map page (6.38): MapPane target-clicked selects the target (ringed), a click on empty map clears the selection; the hit area is 20 px square |
| 1736 | Hover target | tooltip | RELOCATED | kit map page (6.38) row 369: MapPane hovered -> the host's hover-text tooltip (callsign, altitude, speed) |
| 1737 | Zoom in key | button (on map, +) | RELOCATED | kit map page (6.38): the + key bottom right (page-map-zoom-key, half the span), disabled at the limit; no tooltip (the zoom keys carry none, map.slint) and no wheel hint |
| 1738 | Zoom out key | button (on map, -) | RELOCATED | kit map page (6.38): the - key bottom right (twice the span), disabled at the limit; no tooltip |
| 1739 | Pick-receiver cursor | drag/gesture | RELOCATED | kit map page (6.38) row 358: while Set RX here is armed the cursor is a crosshair and the next click sends FOXAPI_OP_SET_POSITION (page-map-clicked); the key is on every map page, not only the satellite window |
| 1740 | Scale bar | overlay/marker | RELOCATED | kit map page (6.38): MapPane's scale bar with the text 'n km' (or 'n m'); the 'at the equator' / 'at N/S' wording is not found in kit_publish.cpp |
| 1741 | Altitude legend | overlay/marker | RELOCATED | kit map page (6.38): MapPane's legend of the altitude bands, drawn while Altitude colours is on; the 'ALTITUDE - kft' heading, the 'no altitude' row and the orbital-altitude legend are not found in kit_publish.cpp |
| 1742 | Markers, labels, trails, coverage ring, receiver mark | overlay/marker | RELOCATED | kit map page (6.38): MapPane's target icons by kind turned to heading, halo labels, the selected / followed / estimated / emergency rings, and the compositor's trails, coverage ring and home rings |

## TOOLS bank — Satellites map window (SATELLITES MAP)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1743 | Plate title | readout | RELOCATED | kit map page (6.38): a satellite plugin opens the generic map page titled '<plugin> map' (rows 1005 and 1006); the SATELLITES MAP plate and its deck of wells are deferred (row 357) |
| 1744 | FIT | key | RELOCATED | kit map page (6.38) row 357: the Fit key; it is never dead and has no 'nothing is plotted' tooltip |
| 1745 | WHOLE WORLD | key | DEFERRED | kit map page row 357: the satellites page's own deck, WHOLE WORLD included, is deferred; the - key backs out a step at a time |
| 1746 | STOP FOLLOWING | key | RELOCATED | kit map page (6.38) row 371: the Stop following key, drawn while following |
| 1747 | Strip text | readout | RELOCATED | kit map page (6.38) row 371: the 'following <id>' text in the toolbar; the idle sentence 'The map moves on its own only while a target is followed.' is not found in pages.slint |
| 1748 | Latitude cell | text field (click to edit) | RELOCATED | the typed latitude is the Radar fold's field (row 1562); the kit map page shows an RX readout, not a click-to-edit cell (6.38 row 358) |
| 1749 | Longitude cell | text field (click to edit) | RELOCATED | the typed longitude is the Radar fold's field (row 1563); the kit map page shows an RX readout, not a click-to-edit cell (6.38 row 358) |
| 1750 | Caption labels | readout | DEFERRED | not found in pages.slint: the RECEIVER POSITION / LATITUDE / LONGITUDE captions belong to the satellites deck, which is deferred (row 357) |
| 1751 | SET FROM MAP CLICK | key (latched while armed) | RELOCATED | kit map page (6.38) row 358: the Set RX here key, lit and reading 'Click the map...' while armed (not a two-line SET FROM / MAP CLICK plate) |
| 1752 | Position note | readout | DEFERRED | not found in pages.slint or kit_publish.cpp: the 'Position set. This window measures...' sentence is not drawn; the RX readout is the only position text |
| 1753 | COVERAGE rocker | toggle | RELOCATED | kit map page (6.38) row 361: the Coverage checkbox (not a rocker with a sub-line; it is not disabled with no receiver position) |
| 1754 | GROUND TRACKS rocker | toggle | RELOCATED | kit map page (6.38) row 362: the Trails checkbox draws the lines plugins publish, ground tracks included |
| 1755 | ALTITUDE COLOURS rocker | toggle | RELOCATED | kit map page (6.38) row 363: the Altitude colours checkbox |
| 1756 | TRAIL STYLE segments | radio (3 segments) | DEFERRED | kit map page row 364 stays deferred: no LINE / RIBBON / OFF choice (the satellites deck is deferred) |
| 1757 | COVERAGE RESET | button | RELOCATED | kit map page (6.38) row 366: the Reset coverage key |
| 1758 | Coverage note | readout | RELOCATED | kit map page (6.38) row 367: the coverage status sentence ('n of m bearings, best k km') |
| 1759 | TARGETS header | readout | RELOCATED | kit map page (6.38) row 359: the target count in the toolbar; there is no TARGETS caption |
| 1760 | SORT segments | radio (4 segments) | RELOCATED | kit map page (6.38) row 369: one Sort key that steps through the eight sort keys (the register's CALLSIGN / NORAD / ALT / AGE segments are not drawn) |
| 1761 | ASC / DESC | button | RELOCATED | kit map page (6.38) row 369: the Up / Down key |
| 1762 | Register rows | list | RELOCATED | kit map page (6.38) row 369: the Table (Callsign, ID, Note); a click selects and centres, a double-click follows (the satellites window centres without following); the ALT and AGE columns are not drawn |
| 1763 | Empty text | readout | RELOCATED | kit map page (6.38) row 369: the generic empty text; the satellites sentence 'Decoded and propagated tracks appear here' is not found |
| 1764 | Selected-target card | readout | RELOCATED | the Target details body (rows 315 and 374) in the drawer or as a page, fed by the selected target; the card's cells and the hatched 'cannot be computed' values are not found in pages.slint or output.slint |
| 1765 | Notes | readout | DEFERRED | not found in pages.slint or output.slint: no NEXT PASSES, hatched-value or inclination / period notes; the satellites deck is deferred (row 357) |
| 1766 | Map key | overlay/marker | DEFERRED | not found in map.slint, pages.slint or kit_publish.cpp: the EQUIRECTANGULAR / NATURAL EARTH / RINGED MARK / DASHED LINE key is not drawn; the nearest thing is the attribution line and the altitude legend |
| 1767 | Map | (shares the Map canvas gestures above) | RELOCATED | kit map page (6.38): MapPane, with the gestures of rows 1733 to 1739 |

## TOOLS bank — Map rows and Target details (reached from here; live in the DECODE bank)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1768 | Map row | key (switch row) | RELOCATED | the DECODE bank's map row (6.38 row 318): a Section per map page, '<plugin> map', chip 'n TGT', lamp lit while open, with the satellites tooltip for satellite plugins |
| 1769 | No-maps row | readout row | RELOCATED | the DECODE bank's blocked 'Target maps NONE' row and its note (6.38 row 319) |
| 1770 | Target details section | key (collapsible) | RELOCATED | the DECODE bank's Target details drawer (6.38 rows 312-315): a Section with the id chip and a state dot |
| 1771 | Go to on map | button (small) | RELOCATED | the Target details Go to on map key (6.38 rows 316 and 375) |
| 1772 | Follow / Stop following | button (small) | RELOCATED | the Target details Follow / Stop following key (6.38 rows 317 and 376) |
| 1773 | TARGET DETAILS window | window | RELOCATED | the Target details window as a docked page or a torn window (6.38 row 373) |
| 1774 | Close | button | RELOCATED | the Target details Close key on the page form (6.38 row 377) |
| 1775 | Display settings (not here) | (none) | RELOCATED | the SYSTEM bank's Display body: Aircraft icons (row 1397) and Trail width (row 1398) |

Subtotal: PRESENT 88, RELOCATED 75, DEFERRED 89, total 252 (rows 1524 to 1775).

# Phase 3: the EXTEND bank, the Plugin store, the Fitted modules window and the patch rows (docs 6.42) - `inventory/extend-system-store-patch.txt` lines 86-286 and 1380-2599 walked row by row

Every control of `inventory/extend-system-store-patch.txt` (FoxSDR 0.99.104) outside the SYSTEM part (lines 287-1379, ticked above in rows 1387 to 1523): lines 86-286 (the EXTEND bank: Web access, CAT control), 1380-1776 (the Plugin store window), 1777-2001 (the Fitted modules window, its module page and the retired rows), 2002-2492 (the Patch row and canvas) and 2493-2599 (the Patch presets page); 188 rows. The inventory file blanks every label string it quotes, so the wording is not compared here (the family's wording is the ImGui window's own, `app_window.cpp` `drawWebSection` / `drawCatSection` and `plugin_store_view.cpp`). The EXTEND bank and the store are `src/slint/ui/extend_bank.slint` (`WebBody`, `CatBody`, `StorePage` with its top bar, SITES panel, Browse grid with `StoreCardView` and `CardGrid`, foot, Updates pane with `UpdateRowView`, clean-up panel and plugin page), their effects (`src/slint/extend_logic.hpp`, the callbacks-to-effects table in its header) and the places `src/slint/ui/main.slint` and `pages.slint` put them (bank 3, rows 0 and 1: Web access and CAT; the store is the body of a `PageTab` of kind 9, `PageTab.store`, `StorePage`). The status words are phase 1's: **PRESENT** = the family draws it and its callback has an effect in `extend_logic` (an op, a local change the publisher keeps as a draft, or both; the note names the component and the effect), **RELOCATED** = it lives elsewhere in the Slint window (the Section row `main.cpp` draws, the DECODE bank's catalogue field and Disabled cards, the kit's fitted page and module page of phase 2, the tab row), **DEFERRED** = not drawn, drawn without an effect, or drawn wrongly, with the reason. A control whose status could not be established from the files is DEFERRED with "not found in" and the file. The host's publisher was being written when this part was ticked: `extend_publish.hpp` had just appeared (no `.cpp`, and `extend_publish` is in no source list of `src/slint/CMakeLists.txt`), and `kit_publish.cpp` had begun to carry the page (`blankStore`, `fillStore`, `setStoreModel`, the DECODE bank's Plugin store row opening page kind 9; the phase-2 rows 278 to 280 are not edited here). PRESENT here therefore means drawn and given an effect in `extend_logic`, not seen working in the window; the two EXTEND Section rows are still the ones `main.cpp` (`sectionsFor`, case 3) draws, with a chip of 'PORT n' or 'OFF' and no lamp.

The departures the family's header states, as they bear on the rows: UPDATE ALL sends FOXAPP_OP_STORE_ADD_ALL with ival[1] = 1 (updates only), as the ImGui key does, and not FOXAPI_OP_STORE_UPDATE_ALL (0x0805), whose engine handler starts the whole ADD ALL run; the family has no REMOVE for an installed plugin (the ImGui store window has none; REMOVE is the Fitted modules page's key); CANCEL (`store-cancel`) and the maker's notice tick (`store-plugin-notice`) are callbacks the family ADDED to the contract; the amber `StoreCard.key-tooltip`, `StoreSite.status`, `add-reason`, `site-error` and the plugin page's `picture-index`, `pictures-note`, `notice`, `reach-lead` are ADDED fields. The Fitted modules window (31 rows: top bar and list, module page, retired rows) is not built in `extend_bank.slint`: its rows are RELOCATED to the plugin kit's fitted page and module page (phase 2, docs 6.38, rows 378 to 406) and the DECODE bank's Disabled cards (rows 281 to 288), with the phase-2 row number named; where the phase-2 row is itself DEFERRED the row here is DEFERRED. The 'Updates footnotes' row (inventory line 1586) is the store's and is ticked here. The inventory's RECEIVER / PATCH view keys (line 1995) sit inside the Fitted modules line range but belong to the Patch view; they are ticked first in the Patch block. The DECODE bank's own Plugin store row (phase 2 rows 278 to 280), the 'Entry:' paragraphs and the 'Not covered / uncertain' notes (inventory lines 2593 to 2599) are not control rows and are not ticked here. Not wired anywhere in the family: Esc on a plugin page returns to the list (the 'Entry' paragraph and the '< BROWSE' row; no Esc handling in `extend_bank.slint` or `pages.slint`), the framed screenshot strip and its arrows, the hatched unknown values of the facts grid, and the store's narrow top bar and its too-narrow and too-small messages. The Patch view (top band, information pane, canvas, node faces, inspector drawer) is `PatchPage` in `patch_page.slint` and the PATCH PRESETS page is `PatchPresetsPage` there, as page kind 10 of the kit (`pages.slint`); the host is `patch_logic.hpp` / `patch_logic.cpp` (the draft, the canvas interaction, the models, one method per callback) and `patch_publish` is the wiring. Three rows stay DEFERRED: the Spectrum face (row 1924), the Map face (row 1925) and the Map inspector's Fit to targets / Whole world keys (row 1940): the nodes exist, move, wire, resize and close, and their faces draw the window's sentences.

## EXTEND bank — Web access (EXTEND rail row, F4 bank)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1776 | Section header chip | readout + lamp | RELOCATED | the Section row 'Web access' of bank 3 (main.cpp sectionsFor): its chip is 'PORT n' (green) or 'OFF' from the SERVERS list and its dot is off; the refused word (FAIL, with the rust lamp) is not drawn: main.cpp keeps webRefused and never reads it, and WebModel's host extras chip, chip-tone and lit are not read by the row yet |
| 1777 | Serve a browser page | checkbox | PRESENT | WebBody Tick -> web-serve -> applyWebServe: [local] draft enabled and dirty; nothing is sent until Apply |
| 1778 | Reachable from | dropdown/combo | PRESENT | WebBody DropKey over model.reach -> web-reach(i) -> applyWebReach: [local] draft bind (this machine only, every network interface, or the specific address, offered only while the config holds one), dirty |
| 1779 | Address readout | readout | PRESENT | a Line 'address' with a Readout, drawn only while model.address is not empty (the specific address chosen) |
| 1780 | Port | number field | PRESENT | LabelledField (mono, right) -> web-port on Enter or focus loss -> applyWebPort: [local] draft port text, dirty, clamped to 1024-65535 when sent; a plain text field with no step arrows |
| 1781 | User name | text field | PRESENT | LabelledField -> web-user -> applyWebUser: [local] draft user, dirty |
| 1782 | Password status | readout | PRESENT | Note password-status, green while a password record exists |
| 1783 | Password | text field (masked) | PRESENT | LineInput with hidden -> web-password -> applyWebPassword: [local] draft only, never echoed or logged; ADDED: a SHOW / HIDE key reveals both password fields |
| 1784 | Confirm | text field (masked) | PRESENT | LineInput with hidden -> web-confirm -> applyWebConfirm: [local] draft confirm |
| 1785 | Set password | button | PRESENT | KeyFlow key 1; commits the two typed fields first -> web-set-password -> applyWebSetPassword: a mismatch is a [local] draft error with both boxes wiped, else FOXAPI_OP_SERVER_CONFIG with the password in its JSON text (an ask ServerConfig; scrubEffect wipes it after the send), boxes wiped |
| 1786 | Clear password | button | PRESENT | KeyFlow key 2, drawn only while clear-label is not empty (a password record exists) -> web-clear-password -> applyWebClearPassword: SERVER_CONFIG with clearPassword |
| 1787 | Apply | button | PRESENT | KeyFlow key 3: lit 'Apply' while apply-enabled, greyed 'Applied' otherwise; commits the port and user fields first -> web-apply -> applyWebApply: SERVER_CONFIG enabled, bindAddress, port, username |
| 1788 | Error line | readout | PRESENT | Note error in the error tone: this window's complaint or the engine's refusal (applyServerAnswer); the model leaves the serving lines empty while it is set |
| 1789 | Serving note | readout | PRESENT | Note serving-note (green): 'serving at http://127.0.0.1:n' or 'serving on port n to every machine on your network' |
| 1790 | Network warning | readout | PRESENT | Note network-warning (error tone) and one mono Note per entry of addresses |
| 1791 | Plain-HTTP note | readout | PRESENT | Note http-note (warn tone), same condition as the network warning |
| 1792 | Session count | readout | PRESENT | Note sessions ('n browser session(s)'), set while the server runs and a login is required |
| 1793 | Audio listeners | readout | PRESENT | Note listeners ('n listening to audio'), set while it runs and someone listens |
| 1794 | Not serving | readout | PRESENT | the same serving-note line: readWeb sets 'not serving' when enabled, not running and no error; it is drawn in the serving note's green, with no tone of its own |

## EXTEND bank — CAT control (rigctld)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1795 | Section header chip | readout + lamp | RELOCATED | the Section row of bank 3 row 1, named 'CAT' (the inventory's 'CAT control (rigctld)'): its chip is 'PORT n' or 'OFF' and its dot is off; the FAIL word, the 'n CLI' client count and the lit lamp are CatModel host extras (chip, chip-tone, lit) the row does not read |
| 1796 | Intro text | readout | PRESENT | CatBody Note intro |
| 1797 | Accept CAT connections | checkbox | PRESENT | CatBody Tick -> cat-accept -> applyCatAccept: SERVER_CONFIG cat enabled at once, [local] draft |
| 1798 | Port | number field | PRESENT | LineInput (mono, right) -> cat-port on Enter or focus loss -> applyCatPort: SERVER_CONFIG cat port clamped to 1024-65535 (restarts the listener); text that is no number sends nothing |
| 1799 | Reachable from other machines | checkbox | PRESENT | CatBody Tick -> cat-reachable -> applyCatReachable: SERVER_CONFIG cat bindAll |
| 1800 | No-password warning | readout | PRESENT | Note warning (warn tone), set by the model only while Reachable is ticked |
| 1801 | Status line | readout | PRESENT | Note status in status-tone (error tone for the refusal); the model holds exactly one of the refusal, 'Listening on ...' and 'Not listening.' |

## Plugin store — window: rail rows, top bar, Browse and Updates lists

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1802 | Rail catalogue URL | text field | RELOCATED | the DECODE bank's catalogue URL Field (phase 2 row 281, FOXAPP_OP_SET_CATALOGUE_URL); the store page does not repeat it |
| 1803 | Rail "Disabled" rows | list | RELOCATED | the DECODE bank's Disabled cards (phase 2 rows 283 to 288), ticked again in the retired-modules block below |
| 1804 | Too-narrow message | readout | DEFERRED | the narrow top bar and its message: not found in extend_bank.slint; StorePage lays out at any width (cards 1, 2 or 3 to a row by the inner width, the search box never under 80 px) and the tab group and the three keys keep their places; no message is drawn |
| 1805 | Too-small message | readout | DEFERRED | not found in extend_bank.slint: no message for a face under 40 px |
| 1806 | Search | text field (with loupe) | PRESENT | SearchBox (loupe, placeholder 'Search the catalogue') -> store-search on each edit -> applyStoreSearch: [local] draft search, a plugin page closed |
| 1807 | BROWSE tab | key (latched) | PRESENT | SegKey in the SegGroup, lit while tab is 0 -> store-tab(0) -> applyStoreTab: [local] tab, page closed |
| 1808 | UPDATES (n) tab | key (latched) | PRESENT | SegKey 'UPDATES (n)' (n from the model's updates), lit while tab is 1 -> store-tab(1) -> applyStoreTab |
| 1809 | Catalogue line | readout | PRESENT | Note under the bar in the model's tone (amber for 'could not refresh', muted otherwise); it wraps, so the full text needs no hover |
| 1810 | CHECK NOW / CHECK AGAIN | button | PRESENT | Key check-key -> store-check -> applyStoreCheck: FOXAPI_OP_STORE_FETCH; greyed while a transfer or a picture is in flight or the catalogue URL is empty (StoreModel.check-enabled, docs 6.61; test_slint_extend) |
| 1811 | SITES | button | PRESENT | Key sites-key, lit while the panel is open, always enabled -> store-sites-toggle -> applyStoreSitesToggle: [local] sites_open |
| 1812 | CANCEL | button | PRESENT | ADDED: a danger Key drawn only while cancel-label is not empty -> store-cancel (ADDED, not in the contract) -> applyStoreCancel: FOXAPI_OP_STORE_CANCEL while busy |
| 1813 | Install/source error | readout | PRESENT | Note top-error (error tone) and top-warning (warn tone) at the top of both lists, hidden while a plugin page is open |
| 1814 | Empty-state text | readout | PRESENT | Note empty-line (muted); the red 'catalogue could not be read' variant (empty_tone) has no Slint field, so every variant is drawn muted |
| 1815 | Site status lines | readout | RELOCATED | drawn per site inside the SITES panel (StoreSite.status, ADDED, amber), not at the top of both lists as the window does: with the panel closed no site fault shows, and the field holds one string where the model has two or three lines (site_lines) |
| 1816 | Category heading | readout | PRESENT | a Caption per StoreCategory over its CardGrid; the host leaves empty categories out |
| 1817 | Plugin card | card | PRESENT | StoreCardView in a CardGrid: glyph tile, name, summary, badges, one key; a press on the card -> store-card-open(id) -> applyStoreCardOpen: [local] page opened (tab 0, details closed, notice tick off) and FOXAPP_OP_STORE_PICTURES_WANT; 1, 2 or 3 columns by the inner width (400 and 612 px) |
| 1818 | Card key GET | button | PRESENT | StoreKey kind 0 -> store-card-key(id) -> applyStoreCardKey: GET with a legal notice and no tick opens the page, otherwise FOXAPI_OP_STORE_INSTALL; a greyed key shows the reason as its tooltip (StoreCard.key-tooltip, ADDED) |
| 1819 | Card key UPDATE | button | PRESENT | StoreKey kind 1 (lit) with the amber version-note beside it -> store-card-key -> applyStoreCardKey: FOXAPP_OP_STORE_UPDATE; greyed with key-tooltip |
| 1820 | Card key FITTING... | indicator | PRESENT | StoreKey kind 2: a greyed indicator with the progress bar (progress-norm) beside it and the progress line under it; a press is dead (the CANCEL key of the top bar cancels); a transfer whose server sent no length shows an empty bar, not the sweeping line |
| 1821 | Card key INSTALLED | indicator | PRESENT | StoreKey kind 3, greyed, no action |
| 1822 | Badges | readout | PRESENT | BadgeFlow chips from StoreCard.badges (amber THIRD PARTY and EXPERIMENTAL, muted build badges, by tone), wrapping, six slots |
| 1823 | GET EVERYTHING | button | PRESENT | Key get-all at the foot of Browse, drawn while get-all-label is not empty (so not with a search typed), the reason it is greyed as text beside it -> store-get-all -> applyStoreGetAll: FOXAPP_OP_STORE_ADD_ALL with ival[0] the bulk tick and ival[1] = 0 |
| 1824 | GET EVERYTHING note | readout | PRESENT | Note foot-note (the run's progress, 'Cannot add all: ...' or the fetch-and-update count); the notice-count sentence, the sites-left-out sentence and the last run's summary (notice_note, left_out_note, add_all_summary) are host extras with no field in StoreModel and are not drawn on the grid |
| 1825 | Legal-notice tick (bulk) | checkbox | PRESENT | SentenceTick, drawn while bulk-sentence is not empty -> store-bulk-tick -> applyStoreBulkTick: [local] bulk_tick (cleared by watchConsents) |
| 1826 | UPDATES heading | readout | PRESENT | the Updates pane's heading Text (updates-heading) |
| 1827 | UPDATE ALL | button | PRESENT | Key update-all, drawn while updates exist, lit and enabled by update-all-enabled -> store-update-all -> applyStoreUpdateAll: FOXAPP_OP_STORE_ADD_ALL with ival[0] = 0 and ival[1] = 1 (updates only), as the window; not FOXAPI_OP_STORE_UPDATE_ALL |
| 1828 | Update row | card (row) | PRESENT | UpdateRowView: glyph tile, name, amber version-note, THIRD PARTY chip, what's-new line, key; a press on the glyph, name or line -> store-card-open(id) opens the plugin page |
| 1829 | Row UPDATE key | button | PRESENT | Key in the row -> store-update(id) -> applyStoreUpdate: FOXAPP_OP_STORE_UPDATE (a FITTING... row is greyed); UpdateRow has no tooltip field, so a greyed key does not say why |
| 1830 | Updates footnotes | readout | PRESENT | one Note per string of footnotes under the rows ('fetched when you press CHECK AGAIN', the sites-left-out note) |
| 1831 | CLEAN UP OLD VERSIONS (n) | button | PRESENT | Key in the Updates pane's foot, drawn while cleanup-label is not empty, greyed by cleanup-enabled, with the first three old copies and 'and n more' as lines under it (not beside it) -> store-cleanup -> applyStoreCleanup: [local] cleanup_open; the Fitted modules copy of the key is its own row below |
| 1832 | Cleanup confirm | dialog (modal) | PRESENT | a small panel in the page's lower right corner, not a modal popup (title, text, cleanup-lines); it lists what cleanup-lines holds (the first three and 'and n more'), and the full list (cleanup_all) is a host extra with no field |
| 1833 | Remove N files | button | PRESENT | Key (lit, cleanup-yes) -> store-cleanup-confirm(true) -> applyStoreCleanupConfirm: FOXAPP_OP_STORE_CLEAN_OLD ival[0] = 0 and the panel closed; the report line afterwards (cleanup_report) has no Slint field |
| 1834 | Keep them | button | PRESENT | Key -> store-cleanup-confirm(false): [local] panel closed, nothing sent |

## Plugin store — SITES panel (under the top bar when SITES is on)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1835 | Site list | list | PRESENT | the panel's rows (address Text, elided) with 'No sites added.' beside the SITES caption when empty; drawn while sites-open and no plugin page is open; the eight-site limit is the host's list |
| 1836 | REMOVE | button (per site) | PRESENT | Key xs per site with the 'plugins already fitted stay' tooltip -> store-site-remove(i) -> applyStoreSiteRemove: FOXAPP_OP_STORE_SITE (remove); greyed while a transfer runs (StoreSite.remove-enabled, docs 6.61; test_slint_extend) |
| 1837 | Site address | text field | PRESENT | LineInput (placeholder 'https://') -> store-site-draft on each edit -> applyStoreSiteDraft: [local] site_draft, the refusal cleared |
| 1838 | ADD | button | PRESENT | Key greyed while add-reason is not empty (ADDED: the reason is a dim Note under the field) -> store-site-add -> applyStoreSiteAdd: the address checked (a refusal is a [local] site error), else FOXAPP_OP_STORE_SITE and the field and tick cleared |
| 1839 | Consent tick | checkbox (+ clickable sentence) | PRESENT | SentenceTick (the sentence is the click target) -> store-consent -> applyStoreConsent: [local] consent; each ADD clears it |
| 1840 | Site error | readout | PRESENT | Note site-error (ADDED, warn tone), set by a refused ADD |

## Plugin store — Plugin page (opened from a card or an update row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1841 | < BROWSE | button | PRESENT | Key -> store-plugin-back -> applyStorePluginBack: [local] page closed, tab 0; Esc also returns (docs 6.61: key_dispatch escapeStep names the plugin page after the dialog; the pure rule is held by test_slint_host_keydispatch; the live page needs a catalogue and is not drivable offline - UNVERIFIED live) |
| 1842 | Header | readout | PRESENT | GlyphTile, the name, the meta line (maker, version, size, platforms, host) joined to the origin note with a middle dot, and the BadgeFlow badges |
| 1843 | Page key | button | PRESENT | StoreKey at the top right -> store-plugin-key -> applyStorePluginKey: as the card's key with the page's own tick (GET greyed until the notice tick); no amber version-note and no progress line beside it (StorePluginPage has no field for either) |
| 1844 | Install report/error | readout | PRESENT | Note report in report-tone (green, or the error tone) for this plugin only |
| 1845 | SCREENSHOTS | readout + strip | DEFERRED | the framed strip with a caption per frame is not drawn: the page shows ONE picture at a time (Image, contain) in a 132 px frame with one caption line (pictures-note) under it; no horizontal strip, no wheel scroll, no per-frame placeholder with its loading or failure words (PictureFrame.state and word have no Slint field) |
| 1846 | Strip arrows | button | DEFERRED | drawn (a Key either side, greyed at the ends) and given no effect: store-picture(i) -> applyStorePicture returns an empty Effect and the draft holds no picture index, so picture-index (a field the host must set) does not move unless the publisher keeps one; they are drawn always, not only on overflow |
| 1847 | WHAT IT DOES | readout | PRESENT | Caption and Note what-it-does; the 'none' fallback wording is the host's |
| 1848 | WHAT'S NEW | readout | PRESENT | Caption and Note whats-new |
| 1849 | BEFORE YOU FIT IT | readout (amber box) | PRESENT | the amber box (caption and the maker's notice, verbatim), drawn while notice is not empty (the host leaves it empty once the plugin is fitted) |
| 1850 | Notice tick | checkbox | PRESENT | SentenceTick in the box -> store-plugin-notice(bool) (ADDED, the contract has no callback for it) -> applyStoreNoticeTick: [local] notice_tick, which enables GET |
| 1851 | DETAILS | readout | PRESENT | the DETAILS caption row |
| 1852 | SHOW DETAILS / HIDE DETAILS | button | PRESENT | Key xs -> store-plugin-details(!open) -> applyStorePluginDetails: [local] details_open |
| 1853 | Facts grid | table | DEFERRED | the facts are drawn as ONE mono Note of 'KEY: value' lines (details) while open, not as a table: the hatched unknown values with their reasons and the read-only selectable HOMEPAGE and SHA-256 fields are not drawn (PageFact.hatched and copyable have no Slint field) |
| 1854 | Reach lead | readout | PRESENT | Note reach-lead under the details, drawn while it is not empty |

## Fitted modules — window (rail key Plugins): top bar and list

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1855 | Too-narrow message | readout | DEFERRED | not found in fitted.slint: the kit's Fitted modules page is a docked tab or a torn window that scrolls, with no too-narrow message (its state chips drop to a second line instead) |
| 1856 | Search | text field | RELOCATED | the kit's fitted page (phase 2 row 379): the Search fitted modules box |
| 1857 | State chips (6) | toggle | RELOCATED | the kit's fitted page (phase 2 row 380): StateChips with counts and lamps, each toggles the filter |
| 1858 | RESET WINDOW SIZES | button | RELOCATED | the kit's fitted page (phase 2 row 382): every page back to its default rectangle |
| 1859 | SCAN AGAIN | button | RELOCATED | the kit's fitted page (phase 2 row 381): FOXAPI_OP_PLUGIN_RESCAN |
| 1860 | Verdict line | readout | RELOCATED | the kit's fitted page (phase 2 row 383): the verdict with the folder |
| 1861 | Install/remove result | readout | RELOCATED | the kit's fitted page (phase 2 row 384): the install report / error line |
| 1862 | Empty list text | readout | RELOCATED | the kit's fitted page (phase 2 row 392): the empty-list sentences |
| 1863 | Group heading | readout | RELOCATED | the kit's fitted page (phase 2 row 385): the category headings over the rows |
| 1864 | Module row | card (row) | RELOCATED | the kit's fitted page (phase 2 rows 386 and 387): FittedRowLine with the state word and lamp; a click opens the module page, a double-click the module's own window |
| 1865 | Row STOP / START | button | RELOCATED | the kit's fitted page (phase 2 row 388) |
| 1866 | Row REMOVE | button | RELOCATED | the kit's fitted page (phase 2 row 389): the first step of the two-step delete |
| 1867 | Row CONFIRM | button | RELOCATED | the kit's fitted page (phase 2 row 390): CONFIRM within 5 s |
| 1868 | CLEAN UP OLD VERSIONS | button + modal | DEFERRED | phase 2 row 391 is DEFERRED and still true: FittedModules has the foot key (cleanup-label) but kit_publish.cpp feeds it an empty label and the page-fitted-cleanup handler is empty; the store's confirm panel (row 1832) is not reachable from the Fitted page |

## Fitted modules — Module page (click a row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1869 | < FITTED | button | RELOCATED | the kit's module page (phase 2 row 393): the back key of ModulePage |
| 1870 | Header + START/STOP | button | RELOCATED | the kit's module page (phase 2 rows 394 and 395): the header with its STOP / START key |
| 1871 | ON THIS MACHINE | readout | RELOCATED | the kit's module page (phase 2 row 396): the state lamp and word |
| 1872 | WHAT THIS MODULE REACHES | list | RELOCATED | the kit's module page (phase 2 row 397): the capability rows with the lead and note sentences |
| 1873 | GRANT / REVOKE RECEIVER CONTROL | button | RELOCATED | the kit's module page (phase 2 row 398): GrantKeys |
| 1874 | GRANT / REVOKE RADIO SETTINGS | button | RELOCATED | the kit's module page (phase 2 row 399): GrantKeys |
| 1875 | GRANT / REVOKE TRANSMIT | button | RELOCATED | the kit's module page (phase 2 row 400): GrantKeys with the transmit plate sentence |
| 1876 | Module command keys | button | RELOCATED | the kit's module page (phase 2 row 401): PlateKeys, two per row |
| 1877 | Keep running | checkbox | RELOCATED | the kit's module page (phase 2 row 403) |
| 1878 | Notes | readout | RELOCATED | the kit's module page (phase 2 rows 402 and 404): the state sentence and the orphan, integrity, origin and notice lines |
| 1879 | FILE / PLUGIN ABI / fitted date | readout | RELOCATED | the kit's module page (phase 2 row 405): FactRow lines |
| 1880 | Shared page body | readout | DEFERRED | phase 2 row 406 is DEFERRED: the module page shows only a one-line catalogue note in place of SCREENSHOTS, WHAT IT DOES, WHAT'S NEW and DETAILS; the store's plugin page (rows 1845 to 1854) draws them for the store only |

## Fitted modules — Retired modules ("Disabled" rows on the rail, under the store row)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1881 | Block message | readout | RELOCATED | the DECODE bank's Disabled card (phase 2 rows 283 and 284): the whole message, never cut |
| 1882 | Update | button | RELOCATED | the DECODE bank's Disabled card (phase 2 row 285): the card's primary key when a catalogue id exists |
| 1883 | Remove | button (small) | RELOCATED | the DECODE bank's Disabled card (phase 2 row 287): the first step of the two-step delete |
| 1884 | Confirm delete / Cancel | button | RELOCATED | the DECODE bank's Disabled card (phase 2 row 288): the armed state expires after 5 s |
| 1885 | Enforcement error | readout | RELOCATED | the DECODE bank's enforcement-error line (phase 2 row 282) |

## Patch — rail entries (the main window's other face)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1886 | RECEIVER / PATCH view keys | key (latched pair) | RELOCATED | the RECEIVER and PATCH tabs of the tab row (phase 1 rows 68 and 69), with the pair's two tooltips; view-selected(1) shows the PatchCanvas stub plate, and main.cpp's on_view_selected only sets view-index (no saved last view, no stop of a running patch on leaving) |
| 1887 | Patch row | button (toggle) | RELOCATED | main.cpp sectionsFor(0) row 0 draws the Section row 'Patch' with the chip PATCH RUNNING (amber) or RECEIVER VIEW (row 1212), as the SIGNAL part says; its click folds a body that is empty and does not switch the view (the tab does, row 1211), and the row has no lamp or tooltip of its own |

## Patch — top band: transport, parts bin, decoder row, blueprints

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1888 | START/STOP dome | button | PRESENT | PatchBand run Key (label run-label START/STOP, danger while PatchModel.running, tooltip run-tooltip) -> run-clicked -> PatchPage::run-clicked -> PatchEvents.run-clicked -> patch_logic PatchPage::runClicked = opRun -> FOXAPI_OP_PATCH_RUN (0x0902, 1 when stopped, 0 when running) |
| 1889 | ALL OFF | button (red) | PRESENT | PatchBand ALL OFF danger Key -> all-off-clicked -> PatchEvents.all-off-clicked -> PatchPage::allOffClicked = opAllOff -> FOXAPI_OP_PATCH_ALL_OFF (0x0903) |
| 1890 | PRESETS | button | PRESENT | PatchBand PRESETS Key (on while PatchModel.presets-open) -> presets-clicked -> PatchEvents.presets-clicked; the publisher opens and closes the kit's page kind 10 'patch-presets' (PatchPresetsPage over PageTab.patch-presets) |
| 1891 | Run readout | readout | PRESENT | PatchModel.run-line / run-line-lit: the Lamp and its caption on their own row under the transport keys |
| 1892 | Part keys (7) | button | PRESENT | PatchModel.parts: seven PartKey (label, kind 0..6, enabled, tooltip; a disabled key keeps its tooltip) in a FlowRow -> part-pressed(kind) -> PatchPage::partPressed |
| 1893 | Decoders row | readout + buttons | PRESENT | PatchModel.decoders: one DecoderKey per PATCH_PARTS row (label, index, tooltip) in a FlowRow after the 'Decoders' caption -> decoder-pressed(index) -> PatchPage::decoderPressed; capped at two rows with a fold key under them ("All N decoders" / "Fewer decoders", docs 6.52) since 43 plugins stood the band 380 px tall (6.45) |
| 1894 | No-decoder note | readout | PRESENT | PatchModel.no-decoders, the dim wrapped sentence after the 'Decoders' caption while decoders is empty |
| 1895 | Blueprints | dropdown/combo (tall) | PRESENT | PatchModel.blueprints-shown / blueprints (SelectItem, note = row tooltip) in a DropKey whose closed text is the fixed placeholder; opening fires blueprints-opened -> PatchPage::blueprintsOpened, a pick fires blueprint-picked(index) -> PatchPage::blueprintPicked |
| 1896 | Blueprint note | readout | PRESENT | PatchModel.blueprint-note under the Blueprints row, dim and wrapped |

## Patch — information pane (top-right of the band)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1897 | Verdict | readout | PRESENT | InfoModel.runnable / verdict: one Chip, good when runnable, err otherwise (PatchModel.info) |
| 1898 | Channel level | readout | PRESENT | InfoModel.level-line in the information pane (amber) |
| 1899 | Plan summary | readout | PRESENT | InfoModel.plan-line in the information pane (muted mono) |
| 1900 | Selection / help | readout | PRESENT | InfoModel.none-selected + help (the three sentences), or selection + folded-note (shown while the inspector is folded) |
| 1901 | Node problems | readout | PRESENT | InfoModel.problems: one ProblemLine per line (text, advisory: rust or dim) |

## Patch — canvas (patch_view.cpp)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1902 | Canvas | drag/gesture | PRESENT | PatchCanvasView over CanvasModel (zoom, grid-step / grid-ox / grid-oy, nodes, wires, ghost): one TouchArea -> canvas-pressed / canvas-moved / canvas-released / canvas-left, scroll -> canvas-wheel(x, y, notches-x, notches-y, ctrl, shift) -> patch_logic PatchCanvas press / move / release / wheel over the PatchDraft (pan, zoom, drag, select) |
| 1903 | Zoom out key | button | PRESENT | PatchCanvasView key '-' -> zoom-step(false) -> PatchCanvas::zoomStep (CanvasModel.keys-shown hides the three keys when the canvas is too small) |
| 1904 | Zoom in key | button | PRESENT | PatchCanvasView key '+' -> zoom-step(true) -> PatchCanvas::zoomStep |
| 1905 | Fit | button | PRESENT | PatchCanvasView key Fit -> fit -> PatchPage::fit -> PatchCanvas::fit |
| 1906 | Node (plate) | node/wire (patch) | PRESENT | CanvasNode (id, kind, kind-caption, name, x / y / w / h, selected, blocked, fault, active) drawn by CanvasNodeView; moved by a drag of the header or body (canvas-pressed / canvas-moved on the draft) |
| 1907 | Close key | button | PRESENT | CanvasNode.hot-close draws the close key; the press is canvas-pressed (pressTarget CloseKey) -> PatchCanvas::press removes the node from the draft |
| 1908 | Resize grip | drag/gesture | PRESENT | CanvasNode.hot-grip draws the brass corner and sets the nwse-resize cursor; the drag is canvas-moved with Interaction.resizeNode (resizeNodeTo) |
| 1909 | Ports | node/wire (patch) | PRESENT | CanvasNode.inputs / outputs: PortDot (x, y, tone); a drag from a port is canvas-pressed -> canvas-moved -> canvas-released and Graph::connect on the draft |
| 1910 | Wire | node/wire (patch) | PRESENT | CanvasModel.wires: CanvasWire (path, tone, selected) as Path elements under the nodes, and CanvasModel.ghost for the wire being dragged; a click on a wire selects it in PatchCanvas::press |
| 1911 | Refusal bubble | readout | PRESENT | CanvasModel.refusal (Refusal shown, text, x, y): the rust bubble at the pointer; the host clears it after 2.5 s |
| 1912 | Delete key | key (keyboard) | PRESENT | PatchCanvasView FocusScope: the Delete key -> canvas-delete -> PatchPage::canvasDelete -> PatchCanvas::deleteKey |
| 1913 | Node readouts | readout | PRESENT | CanvasNode.reading / reading-tone / reading2: the node's own readout lines on its plate |

## Patch — node faces (drawn over the plate)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1914 | Radio ON/OFF | button (small) | PRESENT | FaceModel.radio-switch / radio-switch-label: the small ON/OFF Key on the Radio face -> face-switch(node) -> PatchPage::faceSwitch |
| 1915 | Radio device label | readout | PRESENT | FaceModel.device-label on the Radio face |
| 1916 | Radio centre | number field | PRESENT | FaceModel.centre-text: the LineInput on the Radio face, Enter or focus-out -> face-centre(node, mhz) -> PatchPage::faceCentre |
| 1917 | Centre note | readout | PRESENT | FaceModel.centre-note under the centre field (amber) |
| 1918 | Radio status | readout | PRESENT | FaceModel.lines on the Radio face (FaceLine text, tone) |
| 1919 | Channel frequency | number field | PRESENT | FaceModel.freq-text: the LineInput on the Channel face -> face-freq(node, mhz) -> PatchPage::faceFreq |
| 1920 | Demod mode | dropdown/combo | PRESENT | FaceModel.mode-index: the mode DropKey on the Demod face -> face-mode(node, index) -> PatchPage::faceMode |
| 1921 | Demod squelch | checkbox + slider + readout | PRESENT | FaceModel.squelch-on / squelch-norm / squelch-text and gate-shown / gate-text / gate-word / gate-lit on the Demod face -> face-squelch-on(node, on) / face-squelch(node, norm) -> PatchPage::faceSquelchOn / faceSquelch |
| 1922 | Speaker face | readout | PRESENT | FaceModel.lines and lines-after plus level-shown / level-norm (the level bar) on an audio Sink face |
| 1923 | Text-out face | list | PRESENT | FaceModel.lines on a text-out Sink face (the recent lines) |
| 1924 | Spectrum face | readout + trace + waterfall | DEFERRED | the Spectrum face (the main window's spectrum on a node) is bespoke - next allowance; the node exists, moves, wires, resizes and closes, its face draws the sentence 'Wire the radio or a channel here to see its spectrum.' or 'No spectrum yet - its radio is not running.' and an empty well |
| 1925 | Map face | readout + chart | DEFERRED | the Map face (the kit's MapPane on a node) is bespoke - next allowance; the node exists, moves, wires, resizes and closes, its face draws the window's own sentences (the target counts) and an empty well |
| 1926 | Decoder face | readout | PRESENT | FaceModel.have-picture / picture and lines on a Decoder face |

## Patch — inspector drawer (right of the canvas; appears when a node is selected)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1927 | Fold key | button | PRESENT | PatchInspector fold Key ('>>' open, '<<' in the 24 px strip) -> inspector-fold -> PatchPage::inspectorFold (PatchModel.inspector-folded) |
| 1928 | Kind caption + Name | readout + text field | PRESENT | InspectorModel.kind-caption and the Name LineInput (every keystroke) -> inspector-name(text) -> PatchPage::inspectorName |
| 1929 | Radio: Device | dropdown/combo | PRESENT | InspectorRadio.device-items / device-index / device-label in a DropKey; opening fires inspector-device-opened, a pick inspector-device(index) -> PatchPage::inspectorDevice |
| 1930 | Radio: Look for radios | button (small) | PRESENT | InspectorRadio.looking / look-enabled: the 'Look for radios' Key -> inspector-look -> PatchPage::inspectorLook -> FOXAPP_OP_PATCH_LOOK_FOR_RADIOS + FOXAPI_OP_SCAN_DEVICES |
| 1931 | Radio: Centre (MHz) | number field | PRESENT | InspectorRadio.centre-text / centre-note: LineInput, Enter or focus-out -> inspector-centre(mhz) -> PatchPage::inspectorCentre |
| 1932 | Radio: Sample rate | dropdown/combo | PRESENT | InspectorRadio.rate-items / rate-index, or rate-fixed / rate-fixed-text while a recording sets the rate -> inspector-rate(index) -> PatchPage::inspectorRate |
| 1933 | Radio: live readout | readout | PRESENT | InspectorRadio.lines: the live readout and the 'Up to N radios' note |
| 1934 | Channel: Frequency (MHz) | number field | PRESENT | InspectorModel.freq-text: LineInput -> inspector-freq(mhz) -> PatchPage::inspectorFreq |
| 1935 | Demod: Mode + Squelch | dropdown/combo + squelch controls | PRESENT | InspectorModel.mode-index and squelch-on / squelch-norm / squelch-text / gate-* -> inspector-mode(index) / inspector-squelch-on(on) / inspector-squelch(norm) -> PatchPage::inspectorMode / inspectorSquelchOn / inspectorSquelch |
| 1936 | Decoder: Plugin | dropdown/combo | PRESENT | InspectorDecoder.plugin-items / plugin-index / plugin-label in a DropKey -> inspector-plugin(index) -> PatchPage::inspectorPlugin |
| 1937 | Decoder: info | readout | PRESENT | InspectorDecoder.feed-note, fed (amber lines) and shown-note |
| 1938 | Decoder: Plugin settings | list (form) | PRESENT | InspectorDecoder.settings / has-settings: the kit's PluginSettingsForm (forms.slint) placed in the inspector, committed(id, text) -> inspector-setting(id, text) -> PatchEvents.inspector-setting (answered by the publisher over the kit's setting write; patch_logic has no inspectorSetting) |
| 1939 | Map: input count | readout | PRESENT | InspectorModel.map-wired and map-sources (InspectorMapSource count, name) on the Map inspector |
| 1940 | Map: Fit to targets / Whole world | button | DEFERRED | the Map face (the kit's MapPane on a node) is bespoke - next allowance; the inspector's Fit to targets / Whole world keys are drawn by the window only while a MapView exists, so they stay with the face |
| 1941 | Map: help | readout | PRESENT | InspectorModel.map-help, kept verbatim from the window; it names the chart's keys and wheel, which exist only once the Map face does (row 1940) |
| 1942 | Speaker: Sound goes to | dropdown/combo | PRESENT | InspectorSink.items / index / label in a DropKey (audio-sink) -> inspector-sink(index) -> PatchPage::inspectorSink |
| 1943 | Speaker: readouts | readout | PRESENT | InspectorSink.lines and files-note |
| 1944 | Text out: help | readout | PRESENT | InspectorModel.help on a text-out Sink |
| 1945 | Other kinds | readout | PRESENT | InspectorModel.help = 'This node has nothing to set yet.' for a Display node; the artboard's 'Other kinds' card is not a window feature |
| 1946 | Size | readout | PRESENT | the 'Size' caption in the inspector, for every node kind |
| 1947 | Smaller / Larger | button | PRESENT | the Smaller / Larger Keys -> inspector-size(0) / inspector-size(1) -> PatchPage::inspectorSize |
| 1948 | Default size | button | PRESENT | the Default size Key -> inspector-size(2) -> PatchPage::inspectorSize |
| 1949 | Resize hint | readout | PRESENT | the dim sentence 'Or drag the brass corner at the part's bottom right.' under the size keys |
| 1950 | Remove this node | button | PRESENT | the danger Key 'Remove this node' -> inspector-remove -> PatchPage::inspectorRemove |

## Patch — Patch presets (page, 440 x 460; opened with PRESETS, only while the patch view is showing)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1951 | Preset list | list | PRESENT | PatchPresetsPage as PageTab kind 10 'patch-presets' (pages.slint PageBody, PageTab.patch-presets), opened by the PRESETS key (row 1890): PatchPresetsModel.rows (PresetRow text, previous, selected) and empty-shown; click -> presets-row(index) -> PatchPage::presetsRow, double-click -> presets-row-activated(index) -> PatchPage::presetsRowActivated |
| 1952 | LOAD | button | PRESENT | PatchPresetsPage LOAD Key (load-enabled) -> presets-load -> PatchPage::presetsLoad |
| 1953 | SAVE AS... | button | PRESENT | PatchPresetsPage SAVE AS... Key -> presets-save-as -> PatchPage::presetsSaveAs (opens the name row, edit 1) |
| 1954 | RENAME | button | PRESENT | PatchPresetsPage RENAME Key (rename-enabled) -> presets-rename -> PatchPage::presetsRename (opens the name row, edit 2) |
| 1955 | DELETE | button | PRESENT | PatchPresetsPage DELETE Key (delete-enabled) -> presets-delete -> PatchPage::presetsDelete (asks the Delete preset? question) |
| 1956 | Name field | text field | PRESENT | PatchPresetsModel.name-draft: the name LineInput while edit != 0, every keystroke -> presets-name(text) -> PatchPage::presetsName, Enter -> presets-go |
| 1957 | SAVE / RENAME | button | PRESENT | the name row's SAVE (edit 1) or RENAME (edit 2) Key -> presets-go -> PatchPage::presetsGo |
| 1958 | CANCEL | button | PRESENT | the name row's CANCEL Key -> presets-cancel -> PatchPage::presetsCancel |
| 1959 | Result note | readout | PRESENT | PatchPresetsModel.note / note-good under the keys |
| 1960 | Replace preset? | dialog (modal) | PRESENT | PatchPresetsModel.ask-replace / replace-text: the 'Replace preset?' panel over the page (nothing else takes input while it is up) |
| 1961 | REPLACE / CANCEL | button | PRESENT | the question's REPLACE / CANCEL Keys -> presets-replace(true / false) -> PatchPage::presetsReplace |
| 1962 | Delete preset? | dialog (modal) | PRESENT | PatchPresetsModel.ask-delete / delete-text: the 'Delete preset?' panel over the page |
| 1963 | DELETE / CANCEL | button | PRESENT | the question's DELETE / CANCEL Keys -> presets-delete-confirm(true / false) -> PatchPage::presetsDeleteConfirm |

Subtotal: PRESENT 142, RELOCATED 35, DEFERRED 11, total 188 (rows 1776 to 1963).

## SIGNAL bank — Source: the I/Q file row's file dialog, paste and log (rows added after the 0.99.104 inventory; docs 6.63)

The classic window's I/Q-file row got a Browse key, an Open that is the file dialog over an empty box, a paste for a copied file, a cleaned path and a log line for a refused open (claude/iq-file-open-paste-112c87, folded in docs 6.59). These are the Slint row's own, numbered after the last inventory row; the pure decisions are `core/iq_file_box.hpp`'s, the file read is `core/clipboard_files.*`, the submissions are the classic's (SELECT_SOURCE "row:iqfile" and "file:<path>").

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1969 | I/Q file Browse | button | PRESENT | small Key beside Open (iq-browse), drawn only where a file dialog exists (SourceModel::iq_browse): core::pickFile opens beside the path in the box, else in the recordings folder, WAV recordings first; a choice is SELECT_SOURCE "row:iqfile" then "file:<path>" (the classic's openIqFileAt), a cancel changes nothing and is logged ('the I/Q file dialog was closed without a choice'); the choice is logged without its name |
| 1970 | I/Q file Open over an empty box | button (behaviour) | PRESENT | planIqOpen: an empty box (or spaces, or a pair of quotes round nothing) is the file dialog where there is one (never a 'cannot open file: ' with nothing after the colon), else the box's own line 'Type or paste the path of a WAV recording, then press Open.' and a log line |
| 1971 | I/Q file path: a copied file pastes | text field (behaviour) | PRESENT | LineInput files-paste: a FocusScope's capture-key-pressed offers Ctrl+V to the host first (iq-paste -> core::firstFileOnClipboard, the CF_HDROP list Explorer's Copy puts there); true = the box takes the first file's path, false = the box pastes text as ever; logged without the name |
| 1972 | I/Q file path: a quoted path is cleaned | text field (behaviour) | PRESENT | Open tries core::typedIqPath(box): whitespace and ONE pair of double quotes (Explorer's 'Copy as path') off, a quote inside a name stays; the box then shows what is tried |
| 1973 | I/Q open refused: the log | behaviour | PRESENT | the engine's (Engine::pollIqOpen, the same submission the classic makes): 'source: the I/Q file did not open: <reason>' with the path replaced by '(the file)' (PRIVACY.md: the log never carries a file name); the full sentence stays on screen in the Source section's red line |

## SYSTEM bank — Updates: the kill switch's withdrawn notice (rows added after the 0.99.104 inventory; docs 6.63)

The classic window's notice for a copy the update service has withdrawn (field-early-warning; `drawUpdateBanner`, `app_window_updates.cpp`) is not in the 0.99.104 inventory, so these rows are the Slint window's own, numbered after the last inventory row. The facts are the classic's: `UpdateInfo::runningWithdrawn` and `offerWithdrawn` (`core/updater.hpp`), read by `HostDuties::updateStatus`/`updateCardFor` and `readUpdates`.

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1974 | Withdrawn status | readout | PRESENT | Note status-line in the amber tone: 'FoxSDR <running version> has been withdrawn: the FoxSDR team found a problem with it.' (UpdatesFacts::running_withdrawn; website copy, check on); the Updates chip reads RECALL (amber, lit) in place of NEW / IMPT; plain English on purpose, as the classic's notice, so it does not wait for the catalogues |
| 1975 | Withdrawn advice | readout | PRESENT | Note withdrawn-advice under the status: 'We recommend updating to FoxSDR <version>.' while a good build is offered, else 'A fixed version is not ready yet. foxsdr.com will have it as soon as it is.'; a withdrawn RELEASE is never offered (`offer_withdrawn`: no 'is available', no Show it again, no Download) |
| 1976 | Open foxsdr.com (withdrawn) | button (small) | PRESENT | small Key (site-label) -> updates-open-site -> applyUpdatesOpenSite: [local] OpenUrl kHomepageUrl; drawn only while no good build is offered yet |
| 1977 | Not now (withdrawn) | button (small) | PRESENT | small Key (dismiss-label) beside Open foxsdr.com -> updates-withdrawn-dismiss -> applyUpdatesWithdrawnNotNow: apply UpdateDismiss (HostDuties::updateDismissed: the card goes for this launch); Show it again brings it back |
| 1978 | Withdrawn card | card | PRESENT | the update Card at the foot of the sections panel in the warning tone (UpdateCardModel::withdrawn -> main.slint update-withdrawn): the headline is the withdrawn sentence, the note the advice, and the primary key is Download update while a good build is offered and none otherwise (the Updates row carries Open foxsdr.com); never shown for a Store copy or with the check off |

## 3D views (lane A, docs 6.64) - rows added after the 0.99.104 inventory, from the design package's specs/THREED.md and THREED2.md (rows 3001 and up are this lane's; the 3D views of the other lane take another range)

The 3D view is an ADDED view toggle everywhere; the 2D screen keeps everything it had and stays the default. A control or readout the spec marks NEW or [proposed] whose value the host cannot obtain today is not drawn and is listed DEFERRED with what it needs.

### The receiver's 3D SURFACE (THREED.md section 1, tab RECEIVER, VIEW 2D / 3D SURFACE)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 3001 | VIEW 2D | key (segment) | PRESENT | Scene3DViewToggle "2D" in the surface's strip -> View3D.wf-view-selected(false) -> View3DPublisher: the 2D waterfall comes back, the user's split with it |
| 3002 | VIEW 3D SURFACE | key (segment) | PRESENT | Scene3DViewToggle "3D SURFACE" on a small plate over the 2D waterfall's top left (the strength key is on its right) -> wf-view-selected(true); the choice is saved in the interface half (view3d) |
| 3003 | The surface | picture | PRESENT | the live waterfall history (one row of levels a frame, 192 columns, up to 125 s) as a lit height field in an axis box, painted by the CPU renderer (scene3d, waterfall3d_compositor); a hole where the history is not yet that deep; repainted only while the RECEIVER tab is up and something moved |
| 3004 | ELEVATION | slider | PRESENT | Scene3DCameraBar SliderGroup 4 to 89 degrees over View3D.wf-elevation, the well's own property (32 by default) |
| 3005 | AZIMUTH | slider | PRESENT | Scene3DCameraBar SliderGroup -180 to 180 degrees over View3D.wf-azimuth (-58 by default) |
| 3006 | HISTORY DEPTH 30 s | key (segment) | PRESENT | segment of HISTORY DEPTH -> wf-depth-selected(0); the age axis and the rows follow |
| 3007 | HISTORY DEPTH 60 s | key (segment) | PRESENT | the default |
| 3008 | HISTORY DEPTH 120 s | key (segment) | PRESENT | the longest the history keeps (125 s) |
| 3009 | SHADING Height | key (segment) | PRESENT | one hue, lighter with level (the default) |
| 3010 | SHADING Colour ramp | key (segment) | PRESENT | the strength key's own colours (host_logic heat()); the key plate shows the same ramp |
| 3011 | RESET VIEW | key | PRESENT | the camera back to 32 degrees up, -58 around, zoom 1 (the well's reset(); double-click and Home do the same) |
| 3012 | Drag to rotate, wheel to zoom, double-click to reset | gesture | PRESENT | Scene3DWell's TouchArea: 0.45 degrees per pixel, 1.12 per wheel notch, clamped (scene3d.hpp dragged() and zoomed() hold the rules for the host and the tests) |
| 3013 | The well's keys | keys | PRESENT | after a press the well has the focus: the arrows turn by 5 degrees, + and - zoom, Home and 0 reset |
| 3014 | Axis captions and tick values | overlay/marker | PRESENT | "MHz", "Age (s)" and "dB" captions and five ticks per axis as Slint Text at the projected positions of the box's edges |
| 3015 | VFO line down the surface | overlay/marker | PRESENT | an amber line along the surface at the tuned frequency (and on the floor), with its frequency at the near end; none when the VFO is outside the window or there is no data |
| 3016 | User markers | overlay/marker | PRESENT | a dashed ivory line per marker along the surface and its tab ("M1 145.212 MHz", the 2D tab's words) at the far end |
| 3017 | Strength key plate | readout | PRESENT | Scene3DRampKey over the well's top right: the ramp the surface uses, the dB scale and the FLOOR / CEILING foot of the 2D plate |
| 3018 | Foot lines | readout | PRESENT | the 2D waterfall's foot: lines per second and the history, the tuned frequency, mode and decoder |
| 3019 | Empty state | readout | PRESENT | "No spectrum yet - its radio is not running." (the page's own words) while no row of the history is on the grid and the receiver is stopped |
| 3020 | The view and the camera are remembered | setting | PRESENT | AppConfig::view3d, one text for every 3D view (view3d_prefs); the ImGui window carries it through |
| 3021 | Right-click marker menu on the surface | gesture | DEFERRED | the 2D waterfall's right-click menu (Drop marker here at the frequency under the pointer) needs the pixel unprojected onto the surface; the 2D view and the toolbar's Markers key still drop and list markers |

### The radar scope's 3D AIRSPACE (THREED.md section 2, tab SCOPE, VIEW PPI / 3D AIRSPACE)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 3022 | VIEW PPI | key (segment) | PRESENT | Scene3DViewToggleInline "PPI" in the scope's bar (beside the RANGE stepper, only with a receiver position) -> View3D.radar-view-selected(false): the plan-position tube comes back |
| 3023 | VIEW 3D AIRSPACE | key (segment) | PRESENT | -> radar-view-selected(true); the tube is replaced by the airspace, the gauges, the LCD panel and the deck stay; saved in the interface half (view3d) |
| 3024 | The airspace | picture | PRESENT | the aircraft the scope plots (radar_logic's frame decides which), from the antenna: positions by the classic's own range and bearing, altitude in feet, course, trail; painted by the CPU renderer (radar3d_compositor) only while the SCOPE tab is up in 3D |
| 3025 | TILT | slider | PRESENT | Scene3DCameraBar SliderGroup 5 to 85 degrees over View3D.radar-elevation (26 by default); no AZIMUTH slider (the drag and the arrow keys turn it) |
| 3026 | ALTITUDE SCALE x1 | key (segment) | PRESENT | the height's exaggeration, 0.55 of the range at FL400 (the default) |
| 3027 | ALTITUDE SCALE x2 | key (segment) | PRESENT | twice as high |
| 3028 | ALTITUDE SCALE x4 | key (segment) | PRESENT | four times |
| 3029 | RESET VIEW (airspace) | key | PRESENT | TILT 26, azimuth -40, zoom 1 |
| 3030 | Drag to rotate, wheel to zoom, double-click to reset (airspace) | gesture | PRESENT | Scene3DWell: the 2D tube's drag pans, its wheel steps the range, its click selects; in 3D the drag turns the camera and the wheel zooms (the RANGE stepper in the bar still steps the range) |
| 3031 | The well's keys (airspace) | keys | PRESENT | after a press: the arrows turn by 5 degrees, + and - zoom, Home and 0 reset |
| 3032 | Altitude key | readout | PRESENT | Scene3DRampKey down the well's right edge: FL400 to FL0 on the ramp the aircraft are coloured by (the picture is the renderer's own Ramp) |
| 3033 | Aircraft labels | overlay/marker | PRESENT | "BAW123 FL310": the callsign (else the id) and the flight level, in the aircraft's colour, for the selected one and the nearest up to eight |
| 3034 | Range rings, spokes, antenna | overlay/marker | PRESENT | four rings and twelve bearing spokes on the ground, the antenna's pink cross at the middle, a vertical drop line and shadow under each aircraft, a short vector along its course |
| 3035 | Axis ticks and captions | overlay/marker | PRESENT | five ticks per axis (NM east and north from minus the range to plus it, flight levels 0 to 400) and the captions "NM E", "NM N", "FL" |
| 3036 | Corner readouts | readout | PRESENT | the tube's own corner words over the airspace: "n PLOTTED", the range, "MODE ADS-B", the receiver position |
| 3037 | Trails in the air | overlay/marker | PRESENT | the scope's TRAILS option (off / line / ribbon) draws each plotted aircraft's trail through the air at the altitudes it was heard at, coloured by level |
| 3038 | Selection and emergency marks | overlay/marker | PRESENT | a white ring round the aircraft selected on the HOME list, a red one round an emergency |
| 3039 | Unknown altitude | overlay/marker | PRESENT | an aircraft with no altitude stands ON THE GROUND as a grey diamond labelled "NO ALT" - never at an invented height |
| 3040 | Empty state | readout | PRESENT | with nothing plotted the HOME list's own empty title ("NOTHING BEING HEARD" / "NOTHING CURRENT") over the empty box |
| 3041 | The view and the camera are remembered | setting | PRESENT | view3d, name radar: on, azimuth, elevation, zoom, ALTITUDE SCALE |
| 3042 | Click an aircraft on the airspace to select it | gesture | DEFERRED | needs picking (the nearest projected marker); the HOME list still selects, and the selection is drawn |

### The demod scope's 3D TUNNEL (THREED.md section 4, DEMOD SCOPE with input I/Q, VIEW VECTOR / 3D TUNNEL)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 3043 | VIEW VECTOR | key (segment) | PRESENT | Scene3DViewToggleInline "VECTOR" under the DISPLAY row, shown on a baseband input (I/Q or VECTOR); the vector figure is the default and is untouched |
| 3044 | VIEW 3D TUNNEL | key (segment) | PRESENT | -> View3D.tunnel-view-selected(true): the glass is replaced by the tunnel (a choice that outlives the input: AUDIO, SPEC and MPX show their own glass, I/Q and VECTOR the tunnel again) |
| 3045 | The tunnel | picture | PRESENT | the channel's baseband I and Q across, time along, each sample a point, faint rings marking the cross-section; the newest samples the TIME DEPTH asks for, from the scope's own I/Q tap; painted by the CPU renderer only while the window is open on a baseband input with the tunnel chosen |
| 3046 | TIME DEPTH | slider | PRESENT | sixteen detents 2 ms to 250 ms, 40 ms by default, never more than the tap holds (100 ms at the channel's 48 kHz); the readout beside the caption ("40 ms") says the depth in force |
| 3047 | COLOUR BY Time / Level | key (segment) | PRESENT | Time: cyan at the oldest sample to magenta at the newest; Level: the sample's distance from the middle on the viridis ramp |
| 3048 | RESET VIEW (tunnel) | key | PRESENT | azimuth -35, elevation 18, zoom 1 |
| 3049 | Drag to rotate, wheel to zoom, double-click to reset (tunnel) | gesture | PRESENT | Scene3DWell's TouchArea, as the other views' |
| 3050 | The well's keys (tunnel) | keys | PRESENT | after a press: the arrows turn by 5 degrees, + and - zoom, Home and 0 reset |
| 3051 | Small 2D constellation | overlay/marker | PRESENT | the newest 600 samples in the plane, I across and Q up, in a square in the picture's top left corner (the same colours as the tunnel) |
| 3052 | Axis ticks and captions | overlay/marker | PRESENT | five ticks per axis (time in ms from 0 to the depth, I and Q across the range that fits the samples: 0.05 to 10 in 1-2-5 steps) and the captions "time (ms)", "I", "Q" |
| 3053 | The scope's caption and its sentence when nothing arrives | readout | PRESENT | the scope's own caption of the input in the picture's top right corner; "NO SAMPLES - RECEIVER STOPPED" / "NO BASEBAND IN THE TAP" (the scope's words) over the empty tunnel |
| 3054 | EVM readout | readout | DEFERRED | needs the demodulator's error-vector measure per symbol; the host's scope sees samples, not symbols |
| 3055 | SNR readout | readout | DEFERRED | needs a signal-to-noise estimate of the modulation (the channel's own power is the SIG gauge on the radar scope and the receiver's signal readout); no plugin or engine surface publishes one for this tap |
| 3056 | SYMBOL RATE readout | readout | DEFERRED | needs a symbol clock (a decoder plugin's, e.g. a baud rate it published for the tap) |
| 3057 | LOCK lamp | lamp/indicator | DEFERRED | needs a demodulator's lock state for the tap |
| 3058 | COLOUR BY Symbol | key (segment) | DEFERRED | needs symbol decisions; COLOUR BY Level takes its place, true to the samples |
| 3059 | The view and the camera are remembered | setting | PRESENT | view3d, name tunnel: on, azimuth, elevation, zoom, TIME DEPTH, COLOUR BY |

### The Survey Engine page's 3D SURFACE (THREED2.md section 4, the plugin's panel page, VIEW TABLE / 3D SURFACE)

The plugin publishes one row per channel (frequency, width, guess, peak, over floor, seen as "dwells seen / dwells made", band), sorted by activity, and NO HISTORY BY HOUR: the surface is therefore frequency by OCCUPANCY (the Seen share) by level, each channel a ridge as wide as its width, as high as its peak over the floor and as long as its Seen share. The design's hour axis and the controls that need the plugin's configuration or history are not drawn (rows 3071-3076, DEFERRED).

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 3060 | VIEW TABLE | key (segment) | PRESENT | Scene3DViewToggleInline on the page, above the plugin's own table (the default, untouched) |
| 3061 | VIEW 3D SURFACE | key (segment) | PRESENT | -> View3D.survey-view-selected(true); the table is replaced by the surface and the CHANNEL RANK list; saved in the interface half (view3d) |
| 3062 | The surface | picture | PRESENT | the panel's rows read back through their headings (view3d_publish surveyFeedNow, survey3d_logic channelOfCells; a status row is skipped) and drawn as lit ridges in an axis box by the CPU renderer, only while the page is on screen with the surface chosen |
| 3063 | ELEVATION | slider | PRESENT | Scene3DCameraBar, 4 to 89 degrees, 36 by default |
| 3064 | AZIMUTH | slider | PRESENT | -180 to 180 degrees, -52 by default |
| 3065 | RESET VIEW | key | PRESENT | the camera back to its defaults |
| 3066 | Drag to rotate, wheel to zoom, double-click to reset | gesture | PRESENT | Scene3DWell's TouchArea |
| 3067 | The well's keys | keys | PRESENT | after a press: arrows turn by 5 degrees, + and - zoom, Home and 0 reset |
| 3068 | CHANNEL RANK | readout | PRESENT | the busiest six channels, the panel's own cells quoted: rank, frequency ("144.8500 MHz") and Seen ("4/13"); the page's own words, never a bare percentage |
| 3069 | Axis ticks and captions | overlay/marker | PRESENT | five ticks per axis (frequency in MHz across the channels' span, Seen 0 to 100, level in dB from the floor to the strongest peak) and the captions "MHz", "Seen (%)", "dB" |
| 3070 | Rank numbers on the ridges | overlay/marker | PRESENT | the rank at the far end of each of the busiest six ridges, as high as the ridge |
| 3071 | TIME WINDOW 6 h / 24 h / 7 d | select | DEFERRED | needs a time-binned history of the survey; the plugin publishes only its current table |
| 3072 | PERCENTILE FLOOR | slider | DEFERRED | needs the survey's per-bin samples to re-derive a noise floor; the host only has each channel's peak and its level over the plugin's own floor |
| 3073 | MERGE WIDTH | readout | DEFERRED | the plugin's merge radius is a key of its survey.cfg, which the host does not read; no panel row carries it |
| 3074 | SWEEP BAND | readout | DEFERRED | the configured sweep band is in survey.cfg too; the status lines that say it do not reach the host's decoder output in a bounded run (docs 6.50) |
| 3075 | EXPORT CSV | key | DEFERRED | the plugin already writes survey.csv beside survey.cfg; a host-side export of the table needs a save dialog |
| 3076 | Hour (UTC) axis | overlay/marker | DEFERRED | needs the occupancy by hour of day; the surface's depth is the Seen share instead |
| 3077 | The view and the camera are remembered | setting | PRESENT | view3d, name survey: on, azimuth, elevation, zoom |

### The Radiosondes map page's 3D TRACK (THREED2.md section 1, the plugin's map page, VIEW MAP / 3D TRACK)

The host's picture of a balloon is its TRACK (the current latitude, longitude, altitude, course, speed) and the plugin's PATH (the ground trail: latitudes and longitudes only). The ascent's altitudes are therefore RECORDED BY THE HOST from the moment it hears the balloon (sonde3d_logic SondeHistory, every tick, up to 16 balloons and 4000 samples each): the 3D track is what has been heard since this window has been listening, not the balloon's whole flight.

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 3078 | VIEW MAP | key (segment) | PRESENT | SondeViewBar's toggle above the page's toolbar (found by PageTab.view3d == 2: a map page of a plugin whose name says radiosonde); the map, its list and its toolbar are the default and untouched |
| 3079 | VIEW 3D TRACK | key (segment) | PRESENT | -> View3D.sonde-view-selected(true): the list and the map pane are replaced by the 3D track; the page's toolbar (the three band presets, Fit, Set RX here, the checkboxes) stays; saved in the interface half (view3d) |
| 3080 | The track | picture | PRESENT | the heard track as a line through an axis box (km east, km north, altitude km): the ascent coloured by altitude (viridis), a dot at the last heard position with a drop line, a white star at the burst and a dashed pink descent after it, the plugin's ground trail in cyan on the floor, an orange triangle at the receiver; painted by the CPU renderer only while the page is on screen with the 3D track chosen |
| 3081 | ELEVATION | slider | PRESENT | Scene3DCameraBar, 4 to 89 degrees, 22 by default |
| 3082 | AZIMUTH | slider | PRESENT | -180 to 180 degrees, -48 by default |
| 3083 | RESET VIEW | key | PRESENT | the camera back to its defaults |
| 3084 | Drag to rotate, wheel to zoom, double-click to reset | gesture | PRESENT | Scene3DWell's TouchArea |
| 3085 | The well's keys | keys | PRESENT | after a press: arrows turn by 5 degrees, + and - zoom, Home and 0 reset |
| 3086 | Altitude ladder | readout | PRESENT | down the well's right edge: the altitude ramp 0 to 35 km, a tick at the burst's altitude once a burst was seen, a pointer at the balloon's altitude with its figure ("18.5 km"); the box's altitude axis grows past 35 km for a higher balloon |
| 3087 | Legend | readout | PRESENT | a plate: BURST (white), DESCENT (dashed pink), RX (orange), GROUND TRACK (cyan), in the renderer's own colours |
| 3088 | FOLLOW BALLOON | key (latched) | PRESENT | the map page's own follow, on the balloon followed, else selected, else heard last (MapPage::followedId); greyed while no balloon is heard, lit while the page follows it; the 2D page's "Stop following" key shows the same state |
| 3089 | ASCENT RATE | readout | PRESENT | computed from the host's own samples (the altitude change over the last 8 to 90 s), on the legend plate: "ASCENT RATE +5.1 m/s"; absent until 8 s of track |
| 3090 | The balloon's tag | overlay/marker | PRESENT | "S4720113  18460 m  +5.1 m/s": the id, the altitude and the climb rate at the balloon's last heard position; the burst's own label "BURST 31 km" beside the star |
| 3091 | Axis ticks and captions | overlay/marker | PRESENT | five ticks east and north in km over the heard extent (at least 5 km across, padded), an altitude tick every 5 km, and the captions "km E", "km N", "ALTITUDE km" |
| 3092 | Empty state | readout | PRESENT | while no balloon has been heard: the page's own "No targets. Decoded aircraft, ships and stations appear here." over the empty box |
| 3093 | The view and the camera are remembered | setting | PRESENT | view3d, name sonde: on, azimuth, elevation, zoom |
| 3094 | PREDICTED LANDING chip | readout | DEFERRED | needs a landing predictor (a wind model and a descent profile); nothing the host holds predicts one |
| 3095 | TIME SCRUBBER with a play key | slider | DEFERRED | the host's record starts when it began listening, not at the launch; a scrubber over it is a feature of its own |
| 3096 | LAUNCH marker | overlay/marker | DEFERRED | the first position heard is not a launch unless the balloon was on the ground, which the host cannot know; the receiver's own position is drawn as RX |

# Counts (the whole file: phase 1, phase 2 and the SIGNAL, SYSTEM, TOOLS and EXTEND banks of phase 3)

| Status | Rows |
|---|---|
| PRESENT | 1644 |
| RELOCATED | 198 |
| DEFERRED | 136 |
| TODO (unclassified) | 0 |
| Total | 1978 |

Phase 2 alone: PRESENT 890, RELOCATED 34, DEFERRED 11, total 935.

Phase 3, the SIGNAL bank alone: PRESENT 153, RELOCATED 16, DEFERRED 12, total 181 (the five rows 1969 to 1973 are docs 6.63's; the Transmit page, docs 6.58, took 25 rows from DEFERRED and row 1356 from RELOCATED; row 138 - the Space hold - is phase 1's and went from DEFERRED too).

Phase 3, the SYSTEM bank alone: PRESENT 100, RELOCATED 17, DEFERRED 25, total 142 (the five rows 1974 to 1978 are docs 6.63's).

Phase 3, the TOOLS bank and the map rows alone: PRESENT 88, RELOCATED 75, DEFERRED 89, total 252.

Phase 3, the EXTEND bank, the Plugin store, the Fitted modules window and the patch rows alone: PRESENT 142, RELOCATED 35, DEFERRED 11, total 188.

## 3D globe views (lane B, docs 6.65) - rows added after the 0.99.104 inventory, from the design package's specs/THREED.md section 3, THREED2.md sections 2, 3 and 5 and SATVIEW.md (rows 4001 and up are this lane's; the box views of the other lane are 3001 and up)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 4001 | Propagation: VIEW PICTURE / 3D GLOBE | segment key | PRESENT | GlobeViewBar over the picture page of Propagation Tools (globe_views.slint, pages.slint): PICTURE is the default and stays; 3D GLOBE replaces the picture body, the plugin's preset bar and settings fold stay above it, the panel is its own page |
| 4002 | Propagation: the globe | picture | PRESENT | the coastline's land on a lit sphere shaded by the clock's Sun (night side, a smooth terminator), the 30 degree graticule, HOME, the plugin's TARGET locator and the NCDXF/IARU beacons its panel says are on air as skywave arcs lifted to a 300 km shell (drawn 6 times as high), coloured by hop count |
| 4003 | Propagation: DRAG TO ROTATE | gesture | PRESENT | Scene3DWell: a drag turns the globe (the surface follows the pointer), the wheel zooms, double-click / 0 / RESET VIEW reset, the arrow keys turn it 5 degrees; the camera is remembered |
| 4004 | Propagation: BAND select | segment key | PRESENT | ALL, 20 m, 17 m, 15 m, 12 m, 10 m: the bands the NCDXF beacons are on, the only bands the plugin's panel gives paths for; shows the beacons of that band (the target path stays) |
| 4005 | Propagation: GREYLINE | checkbox | PRESENT | the warm twilight band (+/- 6 degrees of solar elevation) along the day / night edge; on by default |
| 4006 | Propagation: PATH legend | chip | PRESENT | four chips: about 1, 2, 3 and 4 or more hops (orange, cyan, pink, green); hops are counted at 4 000 km a hop and always worded "about" |
| 4007 | Propagation: path chips (name, distance, bearing) | readout | PRESENT | one chip per drawn path: the target and each beacon on air, "4U1UN New York City 5 563 km 288°" - the distance and bearing are the host's great circle from HOME |
| 4008 | Propagation: HOME and SUN OVER readouts | readout | PRESENT | HOME POSITION (the plugin's locator, else the receiver position, else "not set") and the sub-solar point for the clock |
| 4009 | Propagation: RESET VIEW | key | PRESENT | the well goes back to its starting camera (an untouched camera centres on HOME) |
| 4010 | Propagation: sample city chips (NYC, SF, Sao Paulo, Sydney, Tokyo, Delhi, Johannesburg) | chip | DEFERRED | needs data: the host has no list of destination cities and the plugin publishes no per-city paths; only its TARGET locator and the beacons on air are real. What would do it: a plugin API addition publishing the destinations (and their MUF / reliability) the plugin computes for the VOACAP page |
| 4011 | Propagation: per-path reliability colour | picture | DEFERRED | needs data: the plugin's VOACAP reliability per path reaches the host only as pixels of its own picture. What would do it: the REL table as panel rows or a path feed |
| 4012 | Propagation: the plugin's plate keys, settings and panel | key | RELOCATED | unchanged: NEXT PAGE / PREV PAGE / REFRESH / NEXT HOUR / PREV HOUR stay on the module page, the 13 settings fields in the fold above the VIEW bar and the "Propagation Tools" panel as its own page |
| 4020 | FT8: VIEW LIST / 3D GLOBE | segment key | PRESENT | GlobeViewBar over the "FT8 - PSK Reporter" panel page: LIST (the plugin's own table) is the default and stays; the decoder output stays on the Decoder output page |
| 4021 | FT8: the globe | picture | PRESENT | a great-circle path from the receiver (the plugin's Locator, else the receiver position) to the locator carried by each decoded message, coloured by band (20 m orange, 40 m cyan, 15 m pink, 10 m green, the others from the palette), the furthest named; stations HEARD - the host has only what the decoder printed, not who reported whom |
| 4022 | FT8: BAND | segment key | PRESENT | ALL and the twelve FT8 bands 160 m to 2 m in rows of five; shows the stations heard on that band |
| 4023 | FT8: TIME WINDOW | segment key | PRESENT | 15 min / 1 h / 24 h of decodes (the host stamps each line from its own UTC time of day); 1 h is the default |
| 4024 | FT8: stations heard readout | readout | PRESENT | "N stations on M bands" from the decodes in the window (one per callsign and band, the newest kept) |
| 4025 | FT8: FURTHEST HEARD | readout | PRESENT | callsign, kilometres and bearing of the furthest station heard, computed from the decodes' locators |
| 4026 | FT8: band legend | chip | PRESENT | a chip for each band that has a station in the window, in the colour of its paths |
| 4027 | FT8: reporter count and countries ("47 reporters - 12 countries") | readout | DEFERRED | needs data: who reported this station (PSK Reporter's spots about it) is not on any surface of the plugin, and countries need a callsign-prefix table the host does not carry. What would do it: the plugin publishing the reports it receives, and a prefix table in the host |
| 4028 | FT8: camera, DRAG TO ROTATE, RESET VIEW | gesture | PRESENT | as the propagation globe: the arrow keys, the wheel and 0 are driven; a locator in the plugin's settings centres an untouched camera |
| 4040 | GOES: VIEW FLAT / 3D GLOBE | segment key | PRESENT | GlobeViewBar over the "GOES HRIT / LRIT image" page: FLAT (the plugin's picture) is the default and stays |
| 4041 | GOES: the globe | picture | PRESENT | the plugin's received picture draped on the sphere as seen from the spacecraft's longitude (the GOES-R fixed-grid navigation; the picture is taken to be the ABI full-disk frame, the disc touching the picture's edges), a dashed disk edge, the sub-satellite square and the receiver; with no picture yet the plain globe and "Waiting for the first image..." |
| 4042 | GOES: SPACECRAFT | segment key | PRESENT | GOES-East (75.2 W) / GOES-West (137.0 W): the operator says which spacecraft sent the picture (both HRIT presets share 1694.1 MHz, so the host cannot tell); East is the default |
| 4043 | GOES: SUB-SAT marker and label | readout | PRESENT | the orange square at the sub-satellite point labelled "SUB-SAT 0.0 N 75.2 W" (the artboard's wording), and the SUB-SATELLITE POINT readout |
| 4044 | GOES: DISK EDGE | readout | PRESENT | a dashed orange circle at the 81.3 degree horizon of a geostationary satellite |
| 4045 | GOES: LOOK ANGLE | readout | PRESENT | elevation and azimuth of the spacecraft from the receiver position (spherical geometry), "below the horizon from the receiver" when it is, or the request for a receiver position |
| 4046 | GOES: camera, DRAG TO ROTATE, RESET VIEW | gesture | PRESENT | starts centred on the spacecraft's longitude; the arrow keys, the wheel and 0 are driven; choosing the other spacecraft takes an untouched camera with it |
| 4047 | GOES: BAND select (ABI 1-16) | segment key | DEFERRED | needs data: the received picture carries no band; the product id is only in the decoder's "image name" text. What would do it: the plugin publishing the band of the picture it holds |
| 4048 | GOES: SEGMENTS progress (14 of 16) | readout | DEFERRED | needs data: the segment counts are in the decoder's output text ("segment n of N") but were not parsed in this pass; a plugin field would be sturdier than parsing prose |
| 4049 | GOES: SATELLITE select GOES 16 / 18 / 19 | segment key | DEFERRED | needs data: the spacecraft the plugin is receiving is in goes.cfg and not published; the host offers the two operational longitudes (East, West) and the operator picks. What would do it: the plugin publishing the spacecraft id of the picture |
| 4060 | Solar system: VIEW PAGES / 3D ORBITS | segment key | PRESENT | GlobeViewBar over the "Solar System View image" page: PAGES (the plugin's four pages) is the default and stays |
| 4061 | Solar system: the orbits | picture | PRESENT | the Sun, the seven other planets on their orbit rings, the Earth and the Moon on a ring about it (not to scale); the host places them from Standish's JPL elements (the method the plugin names), checked against the plugin's own panel for today |
| 4062 | Solar system: SCALE LOG / LINEAR | segment key | PRESENT | LOG (the plugin's knee, 0.1 AU) keeps the inner planets off the Sun; LINEAR is true to scale |
| 4063 | Solar system: SHOW MOON | checkbox | PRESENT | the Moon marker and its ring about the Earth |
| 4064 | Solar system: DATE scrubber | slider | PRESENT | within a year either side of today (the plugin's own elements are valid to 2050); the readout is the date, "TODAY" beside it on today |
| 4065 | Solar system: TODAY | key | PRESENT | back to today's date |
| 4066 | Solar system: planet chips | readout | PRESENT | Mercury to Neptune, "Mars 1.35 AU 85° W": the distance from the Earth and the elongation with its side (E evening, W morning) for the chosen date, and the Earth's distance from the Sun |
| 4067 | Solar system: DRAG TO ROTATE, tilt, zoom | gesture | PRESENT | a drag turns the spin and the tilt, the wheel zooms; the arrow keys, the wheel and 0 are driven |
| 4068 | Solar system: ROTATE LEFT / ROTATE RIGHT / TILT / ZOOM IN / ZOOM OUT plate keys | key | PRESENT | while the 3D view is on the plugin's plate keys drive the 3D camera (and are not sent to the plugin); with the 3D view off, and NEXT PAGE / PREVIOUS PAGE / NEXT CHANNEL always, they go to the plugin |
| 4069 | Solar system: agreement with the plugin's panel | readout | PRESENT | for today the host compares its distances and elongations with the plugin's own panel rows within their rounding and says so, or names the first body that differs |
| 4070 | Solar system: Uranus and Neptune chips, an Earth chip | readout | PRESENT | the artboard proposes six; the panel carries all seven planets, so all seven and the Earth are shown |
| 4080 | Satellites: VIEW FLAT MAP / 3D GLOBE | segment key | PRESENT | GlobeViewBar over the "Satellites map" page: FLAT MAP (the toolbar, the register and the map) is the default and stays |
| 4081 | Satellites: the globe | picture | PRESENT | the satellites the plugin publishes at their altitude (true to scale up to 1 600 km, held there above it), coloured by orbit class (LEO / MEO / GEO), the ground tracks it predicts, the receiver, the line to the selected satellite, the night side shaded for the clock |
| 4082 | Satellites: FOOTPRINT | checkbox | PRESENT | the circle on the ground the selected satellite can see to the horizon (from its altitude) |
| 4083 | Satellites: GROUND TRACKS | checkbox | PRESENT | the ground tracks the plugin publishes, dashed as it says |
| 4084 | Satellites: satellite chips | chip (key) | PRESENT | one chip per tracked satellite in its orbit-class colour; a click selects it (the map page's own selection) |
| 4085 | Satellites: the selected satellite's readouts | readout | PRESENT | SELECTED (label, altitude), SUB-POINT, FROM THE RECEIVER (slant range, elevation, azimuth) - or why it cannot be computed |
| 4086 | Satellites: DRAG TO ROTATE, RESET VIEW | gesture | PRESENT | the arrow keys, the wheel and 0 are driven; the camera starts on the receiver |
| 4087 | Satellites: NIGHT SIDE layer toggle | checkbox | DEFERRED | the night side is always shaded; the switch was not built in this pass |
| 4088 | Satellites: 3D ORBIT PATH | picture | DEFERRED | needs data: the plugin publishes ground tracks (Earth-fixed), not the inertial orbit; lifting a ground track to the altitude would draw something that is not the orbit. What would do it: the plugin publishing orbital elements or an ECI path |
| 4089 | Satellites: time scrubber (-30 min to +90 min, NOW, PLAY) | slider | DEFERRED | needs data: positions at other times - the plugin publishes the present position and one ground track. What would do it: a plugin API addition publishing predicted positions with times, or the elements to propagate them |
| 4090 | Satellites: pass timeline (24 h) | picture | DEFERRED | needs data: the pass predictions exist only as text rows of the plugin's panel. What would do it: the passes (AOS, LOS, maximum elevation per satellite) as typed panel fields or a feed |
| 4091 | Satellites: the satellites deck (position well, layers, targets register, next passes, Doppler, GRANT RECEIVER CONTROL) | page | DEFERRED | the deck stays deferred as before (tick rows 1743-1765): the 3D globe is an added view of the map page, not the deck |
| 4100 | 3D globes: the chosen view, camera and options are remembered | behaviour | PRESENT | one entry each in the framework's view3d setting (propagation, ft8, goes, solar, satellites): the view, the camera and four small choices, written 0.4 s after the last change and read when a view is first used; the 2D view is the default |
| 4101 | 3D globes: the honest empty states | readout | PRESENT | no home / no receiver position, no station heard, no picture, no satellite tracked, no beacon on air: the well says so in the page's own wording and draws what it can; no sample numbers |
| 4200 | Radio pane: R-tag (Select Radio n) | key | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4201 | Radio pane: device name (elides first) | readout | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4202 | Radio pane: frequency (never elided) | readout | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4203 | Radio pane: mode chip | chip | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4204 | Radio pane: RUN, DEC, MUTE, FAIL lamps (captions drop in a short pane) | lamp | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4205 | Radio pane: STOP / START key (Stop Radio n) | key | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4206 | Radio pane: readout row (GAIN, SQL, VOL, RATE, BIAS, decoder chip, PEAK, SPAN) | readout | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4207 | Radio pane: spectrum gestures (click to tune, double-click, wheel, band drag, edge drag, right-click menu) | gesture | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4208 | Radio pane: waterfall gestures (click to tune, pan, wheel, double-click, right-click menu) | gesture | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4209 | Radio pane: splitter between spectrum and waterfall | gesture | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4210 | Radio pane: selection outline in the radio's colour | overlay/marker | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4211 | Radio pane: the compact spectrum (no 76 px header) | well/plot | PRESENT | placed by the radio workspace (docs/multi-radio-records/screen-s3.md, S3): the pane is in the Slint window in a multi-pane layout; drive scripts reach it only with the bounded run's test seam (testradios=N in the script header: radio 1 the engine's, 2 to N made up), because the live multi-radio engine (stage 4) is not in this tree |
| 4212 | Workspace bar: layout keys One / Side by side / 2 x 2 | key | PRESENT | SegGroup in a 30 px bar over the receiver view, shown only while more than one radio is open (with one radio the view is exactly today's); a press asks the host (WorkspaceModel.layout-selected -> Workspace::setLayout) and the host sets the layout back, so the lit key is the layout chosen |
| 4213 | Workspace bar: page keys (Radios 1-4, Radios 5-8; pairs in Side by side) | key | PRESENT | one SegKey per page, only when a radio lives beyond the first page of the chosen layout; the page shown is lit; a press -> Workspace::setPage |
| 4214 | Workspace bar: small-window notice | message | PRESENT | the host's sentence when a multi-pane layout is chosen and the window is below 1280 x 720 (One is shown; the chosen layout returns when the window is wide again) |
| 4215 | Workspace: an empty slot (no radio in that cell) | well/plot | PRESENT | the card "Add a radio" (row 4227) while a slot is free; a quiet well named "Radio n" when no radio can be added (eight are open) |
| 4216 | Workspace: the grid of panes (2 x 2, side by side; a page of four) | layout | PRESENT | RadioWorkspace places the panes of the shown layout, 6 px gaps; a pane exists only while its radio is on screen; the splitter of each radio is kept in WorkspaceModel.splits |
| 4217 | Strip: "Controls follow" radio selector (R1 to R4 of the page shown) | key | PRESENT | a SegKey per radio of the page shown (the block of four that holds the page; with more than four radios it follows the page), the selected one lit, named "Controls follow Radio n"; a press -> RADIO_FOCUS (screen stage S4, docs/multi-radio-records/screen-s4.md). Shown only while more than one radio is open |
| 4218 | Strip: STOP Rn | key | PRESENT | stops the SELECTED radio (RUN 0 aimed at it); live only while that radio runs (its START is the pane key, START ALL, or the strip's own key); shown only while more than one radio is open |
| 4219 | Strip: STOP ALL / START ALL | key | PRESENT | RUN aimed at every open radio (FOXAPI_TARGET_ALL_RADIOS): STOP ALL while any radio runs, START ALL while none does; shown only while more than one radio is open |
| 4220 | Strip: MASTER lamps (Radio n master) | key | PRESENT | one lamp per OPEN radio (up to eight): green running, red failed, dark stopped; a click selects that radio and turns the page to it; the tip says the state in words; shown only while more than one radio is open |
| 4221 | Sections panel: the R-tag of the selected radio | readout | PRESENT | the radio the panel and the banks show, in its colour; only while more than one radio is open |
| 4222 | Audio mix: route key (Output of Radio n) | key | PRESENT | both / left / right / none per radio, one key that cycles; RADIO_AUDIO; read back as fact.radio.<n>.audio; at the foot of the sections panel, only while more than one radio is open |
| 4223 | Audio mix: MUTE (Mute Radio n) | key | PRESENT | SET_MUTED aimed at the radio; lit while the radio is muted |
| 4224 | Audio mix: SOLO (Solo Radio n) | key | PRESENT | RADIO_SOLO; one radio heard alone, a second press hears all again; the engine publishes no read-back, so the lit key is the host's own record (a seam) |
| 4225 | Audio mix: volume slider (Radio n: VOLUME) | slider | PRESENT | SET_VOLUME aimed at the radio |
| 4226 | Status bar: one chip per open radio (Rn, its state, its frequency and mode) | chip | PRESENT | in front of today's chips while more than one radio is open; a click selects the radio; a figure the radio has not given shows "--", never another radio's |
| 4227 | Add a radio: the card in an empty cell | card | PRESENT | the devices not already held, one key each ("Add <device>"), "Look for radios again", and the engine's refusal under the keys as it worded it (RADIO_ADD); in every empty cell of the shown page while a slot is free |
| 4228 | Status bar: "Add a radio" key and its sheet | key | PRESENT | the way to add a radio; the one new thing a one-radio user sees; opens the same card in a sheet (the lowest free slot), shown while fewer than eight radios are open |
| 4229 | Radio pane: close key (Remove Radio n) | key | PRESENT | radios 2 and up; removes a stopped radio at once (RADIO_REMOVE), asks first when the radio runs or records |
| 4230 | Remove question: Remove / Cancel | dialog | PRESENT | the sheet before a running or recording radio is removed |
| 4231 | Workspace bar: Link tuning | key | PRESENT | an option, OFF by default (screen stage S5): linked radios move together by the same amount and keep their gaps; every open radio joins when it goes on, a radio added later joins, a removed radio leaves; lit while on; the tip is the rule in words |
| 4232 | Workspace bar: Start together | key | PRESENT | RUN aimed at every open radio (FOXAPI_TARGET_ALL_RADIOS), one command; live only while some open radio is stopped |
| 4233 | Radio pane: link key (Link Radio n) | key | PRESENT | only while Link tuning is on: takes the radio out of the group or puts it back; lit = in the group, plain = left out |
| 4234 | Radio pane: could not follow (the link key in the danger ink with an amber dot) | indicator | PRESENT | a linked radio that stayed short of the group (a tuning limit): the key's tip says where it stays; the group's intended frequency is kept and the gap comes back when the group moves back |
| 4235 | Workspace bar: the radios' selector, STOP keys and MASTER lamps in a compact window, on every tab | key | PRESENT | the bar wraps its groups onto as many 30 px rows as the width asks (nothing is clipped down to 624 px wide) and, in a window under 1760 px, is over the PATCH, SCOPE and plugin tabs as well as the receiver |
| 4236 | Keyboard: Ctrl+1 to Ctrl+8 select a radio | key | PRESENT | the key table's rows selectRadio1..8 (rebindable, saved, listed in the keys page under "Radio"); a radio that is not open is left alone; the page that holds the radio is shown |
| 4237 | Keyboard: Alt+PageUp / Alt+PageDown turn the page of radios | key | PRESENT | the rows radioPagePrev / radioPageNext; clamped at the ends; where One is shown they select the neighbouring radio; Ctrl+PageUp / Ctrl+PageDown stay "tune one screen width" |
| 4238 | Key bindings: the "Radio" group (Select Radio n, Previous page of radios, Next page of radios) | list | PRESENT | ten rows at the foot of the key table, listed and rebound like every other row |
| 4239 | Saved layout: layout, page, selected radio, each splitter, Link and the radios left out of the group | behaviour | PRESENT | one entry "workspace" in the interface half's view3d setting; restored at start (what the engine restored shows: with one radio the window shows One and radio 1 and the saved eight-radio layout is kept unchanged) |
| 4240 | Per-radio lists: the strip GAIN slider and the SIGNAL bank's gain, antenna, sample-rate and decoder lists are the SELECTED radio's | behaviour | PRESENT | read with read_radio_list by the selected radio's number (host_logic readListFor, not the session's focus); a gain set is aimed at that radio (target = the selection). One radio: unchanged. Screen stage S6, B1. |
| 4241 | DECODE drawer: the Runs-on picker (Runs on, one key per open radio) | key | PRESENT | shown for a decoder plugin only while more than one radio is open (a plugin that transmits stays on radio 1); the key of the radio it runs on is lit (fact.plugin.<file>.radio); a key sends PLUGIN_RADIO 0x1206 (running plugins keep running). Screen stage S6, B2. |
| 4242 | Runs-on picker: a removed radio sends its plugins back to radio 1, stopped | behaviour | PRESENT | the entry shows radio 1 lit and the START key (fact.plugin.<file>.radio and the plugin's stopped flag), the picker goes with the second radio. Screen stage S6, B2. |
| 4243 | Radio pane: the decoder chip of EVERY radio (not only the selected one) | chip | PRESENT | the first decoder fed on that radio, from read_radio_list(DECODERS) for that radio. Screen stage S6, B2. |
| 4244 | Add a radio: I/Q file (path field and Add key) | card | PRESENT | RADIO_ADD file:<path> (an empty field sends nothing). Screen stage S6, B3. |
| 4245 | Add a radio: rtl_tcp server (address field and Add key) | card | PRESENT | RADIO_ADD open-rtltcp:<address>. Screen stage S6, B3. |
| 4246 | Add a radio: Pluto (address field and Add key) | card | PRESENT | RADIO_ADD open-pluto:<address>. Screen stage S6, B3. |
| 4247 | Add a radio: Sound card key (the engine refuses it with its own sentence, shown on the card) | key | PRESENT | RADIO_ADD soundcard: is refused (UNSUPPORTED: the sound card is the first radio's source); the sentence is shown unchanged and the card stays. Screen stage S6, B3. |
| 4248 | Radio pane: why this radio sits on the generator (the engine's refusal / restore sentence over the spectrum) | message | PRESENT | fact.radio.<n>.refusal, else the line of fact.radio.restore for the radio; nothing is drawn when there is none. Screen stage S6, B3. |
| 4249 | Audio mix: SOLO shows the engine's solo (fact.radio.solo) after a restart and when another client changes it | behaviour | PRESENT | read back every refresh instead of a host copy (soloFromFact). Screen stage S6, B4. |
| 4250 | Tuning keys (Ctrl+Up / Ctrl+Down): one step of 10 kHz, as the key table says | key | PRESENT | the Tune step picker starts at 10 kHz (it started at 12.5 kHz while the row said 10). Screen stage S6, B5. |

## SIGNAL bank - Audio filters: the AGC choice and the noise blanker (multi-radio lane C; rows 4400 and up are this lane's, docs/multi-radio-records/agc-nb-wiring.md)

Two audio features of the selected radio, below the Auto notch row of the Audio filters section (the classic window and the web page get no control). FOXAPI_OP_SET_AGC 0x0111 and FOXAPI_OP_SET_NB 0x0112, receiver-scoped.

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 4400 | AGC (Auto, Fast, Medium, Slow, Off) | dropdown | PRESENT | DropKey agc-modes -> filter-agc-mode(index) -> opAgcMode: SET_AGC ival[0] = the mode, ival[1] = 0; the key shows the engine's agcMode |
| 4401 | AGC Fixed gain (live only in Off) | slider | PRESENT | SliderRow, -20 .. +40 dB over 0..1 (whole dB), greyed unless agcMode is Off -> filter-agc-fixed(norm) -> opAgcFixed: SET_AGC with the mode in force (Off) and ival[1] = 1, num[0] = the gain; the engine sets the mode first |
| 4402 | AGC gain in use (dB) | readout | PRESENT | the engine's agcGainDb (a measurement), whole dB, beside the selector; the accessible name is 'AGC gain now: n dB' |
| 4403 | Noise blanker | checkbox | PRESENT | Tick -> filter-nb(on) -> opNb: SET_NB ival[0] = on, ival[1] = 0; off by default |
| 4404 | Noise blanker Threshold | slider | PRESENT | SliderRow, 6 .. 30 dB over 0..1 (whole dB), greyed unless the blanker is on -> filter-nb-threshold(norm) -> opNbThreshold: SET_NB with the switch in force and ival[1] = 1 |
| 4405 | Blanking n % / Standing down | readout | PRESENT | the engine's nbBlankedPct (rounded) as 'Blanking n %', or the amber 'Standing down: signal too strong' while nbStandingDown; shown only while the blanker is on |
| 4600 | FM de-emphasis choice (50 us / 75 us / Off) | dropdown | PRESENT | DropKey in the Audio filters section, the classic's De-emph combo; live in NFM and WFM, greyed elsewhere -> filter-deemph(i) -> opDeemph: SET_DEEMPHASIS ival[0] |
| 4601 | Digit grouping (counter dots 1.090.000.000 / 1090.000.000) | dropdown | PRESENT | DropKey in SYSTEM > Display -> the host's record (workspace entry, dg=); FrequencyCounter.dot-style |
| 4602 | Typed frequency accepts dotted entries in either style | field | PRESENT | parseTypedFrequencyHz: two or more dots are grouping marks; one dot is a decimal MHz as before |
| 4603 | Frequency lock key by the counter | key | PRESENT | Key with a padlock icon, lit while locked -> lock-toggled; per radio, saved (fl=) |
| 4604 | Locked radio: tuning refused, note under the counter | behaviour | PRESENT | lockedApi refuses every tuning command of a locked radio (ops listed in gaps_logic.hpp); the note replaces the counter's caption for three seconds |
| 4605 | Picture page: Save as BMP shows Saving... and greys the key while the file is written | key | PRESENT | KitPublisher: the write runs on a worker (PluginKit::prepareSavePicture); the key is greyed and the note says Saving... until the sentence comes back |
| 4606 | Map page: Line / Ribbon trail style keys | segment | PRESENT | SegKey pair beside Altitude colours; the setting SYSTEM > Display's Trail style saves (mapTrailStyle) |
| 4607 | Counter right-click menu: Enlarge figures / Normal size / Enlarge every reading save their settings | menu item | PRESENT | menu ids 101, 102, 104 save counterScale and readingsScale in the interface half (the figures are not drawn larger yet) |
| 4608 | Counter right-click menu: Copy frequency / Paste frequency | menu item | PRESENT | menu ids 105 and 106; the dotted text in force goes to the clipboard, a pasted frequency (either style) is tuned through the lock |
| 4609 | BROWSE FILES on a plugin PATH setting | key | PRESENT | KitPublisher::browseSetting -> core::pickFile; the chosen path is entered as if typed (editSetting) |
| 4610 | File dropped on the window (recording, frequency list, other) | behaviour | PRESENT | gaps_win's drop target -> handleDrop; .wav opens as the I/Q file, .xml and .csv go to the import, anything else is refused with a note |
| 4611 | RESTART SDRPLAY SERVICE key | key | PRESENT | Key in the Source section while the driver has trouble reaching the service; SdrPlayServiceRestart on a worker (UAC prompt) |
| 4612 | Copy diagnostics | key | PRESENT | SystemPublisher::copyDiagnostics: the bundle to the clipboard and to <reports>/diagnostics.txt; the status line says where |
| 4613 | Help: the manual (app menu) | menu item | PRESENT | menu id 605 opens docs/MANUAL.md of the public repository in the default browser |
| 4614 | Run SDRplay diagnostic and its bias-tee question | key | DEFERRED | the probe child process of the classic window (source/sdrplay_probe) is not ported; the key still says so in the log |
| 4615 | High contrast option and skin | checkbox | PRESENT | Tick in SYSTEM > Display (hc=); the HighContrast skin is a palette; also forced on while Windows' own setting is on |

## Lane S1 (docs/multi-radio-records/screen-s1.md; rows 4500 and up are this lane's)

The status bar and the One layout's plot (the S6 leftovers), one-click layouts, the waterfall as a file, the persistence trace and the decluttered names of plugin marks.

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 4500 | Status bar: Add a radio, Request a feature and Report a bug / dislike keep their words and sit wholly inside the window | behaviour | PRESENT | the keys are a group with the width of their words; the chips live in an area of their own that takes what is left and clips there (main.slint). Read back as the keys' right edge against the window's width in English, French (1280 x 720) and Japanese (800 x 560). Lane S1, item 1. |
| 4501 | Status bar: a chip whose cell does not fit is not drawn (the first chip always is) | behaviour | PRESENT | no half chip is shown at the edge. Lane S1, item 1. |
| 4502 | Spectrum (layout One): the dB labels are laid out over the plot the window gives the spectrum and are at least 14 px apart | behaviour | PRESENT | main.slint reports the plot's size (one-plot-h, one-plot-w), main.cpp lays the dB labels and the frequency ticks over it; read back as figure.plotheight and figure.dbpitch at 800 x 560 and 1280 x 720. Lane S1, item 1. |
| 4503 | Title bar: Layouts key (opens the menu of layouts) | key | PRESENT | LayoutsModel.menu-requested -> MenuKind::Layouts: the stock layouts (Airband, HF DX), the user's own, "Save the current layout..." and "Manage layouts...". Lane S1, item 2. |
| 4504 | Layouts menu: one item per layout (applies it) | menu | PRESENT | layouts_prefs apply(): the bank, then its sections, the rail, the tab, the split, the workspace layout, Link, the selected radio, the 3D waterfall, and (only when saved with "Include tuning") the frequency and mode; it never starts or stops a radio, a recording or a decoder, and does not leave the PATCH tab while a patch runs. Lane S1, item 2. |
| 4505 | Layouts menu: Save the current layout... / Manage layouts... | menu | PRESENT | both open the Layouts sheet. Lane S1, item 2. |
| 4506 | Layouts sheet: Apply (every layout) | key | PRESENT | the same apply as the menu; the note says what was held back (the tab while a patch runs, a selected radio that is not open). Lane S1, item 2. |
| 4507 | Layouts sheet: Rename / Done (the user's own layouts) | key | PRESENT | an inline field; a built-in layout's or another layout's name is refused with a sentence. Lane S1, item 2. |
| 4508 | Layouts sheet: Delete (the user's own layouts) | key | PRESENT | removes it from the settings text at once; the stock layouts cannot be deleted. Lane S1, item 2. |
| 4509 | Layouts sheet: layout name, Include tuning, Save the current layout | field | PRESENT | saves the arrangement under the name (twelve of the user's own at most; the same name replaces); saved in the interface half's view3d text as the raw entry layouts. Lane S1, item 2. |
| 4510 | Layouts sheet: the sentence under the form | message | PRESENT | saved, replaced, renamed, deleted, applied, refused, held back. Lane S1, item 2. |
| 4511 | TOOLS bank: Export section | section | PRESENT | row 5 of the TOOLS bank, closed by default. Lane S1, item 3. |
| 4512 | Export: Save to (folder and file name) | field | PRESENT | typed; a relative path goes under the profile's exports folder, a missing extension is added, an existing file is never overwritten ("a (2).png"). Lane S1, item 3. |
| 4513 | Export: Data length (1, 5, 15, 60 minutes) | key | PRESENT | how much of the radio's store of levels a data file takes. Lane S1, item 3. |
| 4514 | Export: Data format (CSV, FSW) | key | PRESENT | CSV or the FoxSDR spectrum file. Lane S1, item 3. |
| 4515 | Export: Save waterfall picture | key | PRESENT | the waterfall as drawn plus a frequency scale, a time scale and a caption, as a PNG with the FoxSDR text chunk; written by a core::DiskJob, off the frame. Lane S1, item 3. |
| 4516 | Export: Save waterfall data | key | PRESENT | the store's rows of the chosen length as CSV or FSW, written off the frame. Lane S1, item 3. |
| 4517 | Export: what the store holds and what the last file did | readout | PRESENT | "Radio n at ..., span ...: m min stored, x MB" and the sentence of the last save. Lane S1, item 3. |
| 4518 | Spectrum toolbar: Trace Persistence | key | PRESENT | the fourth trace choice: the trace of the last seconds as a density that decays. Lane S1, item 3. |
| 4519 | Trace menu (right-click): Persistence and Persistence decay (Fast, Medium, Slow) | menu | PRESENT | decay time constants 0.3 s, 1 s, 3 s. Lane S1, item 3. |
| 4520 | Plugin marks: the names are stacked into rows instead of drawn over each other (spectrum and waterfall) | behaviour | PRESENT | mark_layout.hpp: up to three rows; every mark keeps its line. Lane S1, item 4. |
| 4521 | Plugin marks: names that fit no row are counted into a "+N" tab; a click lists them and a pick tunes there | key | PRESENT | on the spectrum, on the waterfall and in the radio panes. Lane S1, item 4. |
| 4522 | Plugin marks: a name's tooltip (the name, its frequency, the plugin it came from) | tooltip | PRESENT | drawn inline in the shared tooltip face. Lane S1, item 4. |
## Screen lane C - tuning, scanning, memories and controls (rows 4800 and up are this lane's; docs/multi-radio-records/screen-c.md)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 4800 | Scanner: Lock out 10 min | key | PRESENT | Key under the Scanner body, live while a scan of the ticked rows runs -> LaneC.act lock-temp: the row the scan is on is unticked (FOXAPP_OP_BOOKMARK_TICK) and ticked again after ten minutes; a range scan has no row (the engine hand-off: Params::lockedHz) |
| 4801 | Scanner: Lock out for good | key | PRESENT | Key: as Lock out 10 min with no end -> lock-perm (the row stays unticked until Clear lockouts) |
| 4802 | Scanner: Clear lockouts and the n locked out count | key | PRESENT | Key (live while one is out) and the dim count beside it -> lock-clear ticks back every row this window unticked |
| 4803 | Scanner: Priority MHz field | field | PRESENT | LabelledField -> priority: the typed list in MHz (comma separated, bad tokens dropped, at most 16), saved in lanec.json; the engine does not read it yet (a note says so) |
| 4804 | Scanner: Every s field | field | PRESENT | LabelledField -> priority-every: seconds between priority visits, clamped 5..600, a non-number goes back to the stored value |
| 4805 | Scanner: Mute while scanning | checkbox | PRESENT | Tick -> mute-scan, saved; the follower sends SET_MUTED 1 while a scan steps and 0 when it holds a signal or ends (a mute the user set is never touched) |
| 4806 | Scanner: Log what the scan finds | checkbox | PRESENT | Tick -> log-scan, saved: one line per signal the scan stops on (time UTC, frequency, seconds heard) is appended to scan-log.csv beside the settings file |
| 4807 | Scanner: what the scan found; the engine note | readout | PRESENT | dim line Found n on m frequencies; most heard ... and an amber note while the engine does not read the priority list and the thresholds |
| 4808 | Bookmarks: memory of the tuned frequency, Squelch dB | field | PRESENT | LabelledField + the line above it naming the bookmark the receiver is on (greyed with the sentence that says why when it is on none) -> mem-squelch; a recall (tuning back onto the bookmark, no scan running) sends the entry's squelch |
| 4809 | Bookmarks: memory, Scan threshold dB | field | PRESENT | LabelledField -> mem-threshold: the entry's scan threshold in dB (Scanner::Params::thresholdDb when the engine reads it) |
| 4810 | Bookmarks: memory, Offset kHz and the Transmit line | field | PRESENT | LabelledField -> mem-offset: repeater shift, signed kHz; the note under it says the transmit frequency |
| 4811 | Bookmarks: memory, Tone Hz | field | PRESENT | LabelledField -> mem-tone: CTCSS tone 60..260 Hz or empty; stored, not yet applied (the tone squelch waits for F14) |
| 4812 | Bookmarks: Store into a memory and the eight memories M1 to M8 | checkbox + key | PRESENT | Tick -> quick-store, then a press on a memory stores the frequency, mode and width the receiver is on; a press on a stored one recalls (frequency, mode, width); the key then reads M2  100.300 |
| 4813 | Counter menu: Lock the tuning step | menu item | PRESENT | Checked item 107 in the counter's right-click menu: every digit and the wheel step by the Tune step; saved |
| 4814 | Counter menu: Delta lock | menu item | PRESENT | Checked item 108, per radio, saved: click-to-tune on the waterfall becomes a retune of the counter (the VFO offset is kept, the centre moves); the VFO offset itself is refused |
| 4815 | Counter menu: Remember the width of each mode | menu item | PRESENT | Checked item 109, saved: the width each mode was last used at is kept once it has settled and given back when the mode is entered again |
| 4816 | Airband channel name under the counter | readout | PRESENT | Channel 118.000 / 118.005 or Channel 118.010 in the place of the converter caption when the frequency is an airband channel (core::airbandChannelName) |
| 4817 | Status bar clock | key | PRESENT | dim clock at the right of the status bar, UTC or local time (LT); a press cycles UTC, local, off (--:--:--); saved |
| 4818 | Keyboard shortcuts overlay | sheet | PRESENT | Shift+F1 or App menu > Keyboard shortcuts: every bound action of the saved key table with its chord; Esc or Close closes it |
| 4819 | Recordings sheet: the list, Open, Open folder | sheet | PRESENT | App menu > Recordings...: the recorder's folder, newest first, kind (I/Q or Audio), size, date; Open plays an I/Q take as the I/Q file and hands any other file to the system; Open folder shows the folder |
| 4820 | Decoder output: Copy key | key | PRESENT | Key in the Decoder output bar -> every line the window holds, as text, to the clipboard |
| 4821 | Picture page: wheel zoom, drag to pan, Fit key | behaviour | PRESENT | The wheel zooms the plugin's picture about the pointer (up to 16 times), a drag pans while zoomed, Fit shows it whole; a click is still the picture's own pixel under the pointer |
| 4822 | Shift+F1 fixed shortcut | shortcut | PRESENT | not in the key table (F1 is the SIGNAL bank's): answered after the table by key_dispatch |

## Lane B (docs/multi-radio-records/screen-b.md; rows 4700 and up are this lane's)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 4700 | Display body: Colour map (the waterfall's six maps: Classic and five OKLab maps; the strength key follows) | dropdown | PRESENT | S7. spectrum_extras paletteTable; WaterfallLook.ramp Lane B. |
| 4701 | Display body: Waterfall speed (every frame down to a line a minute; a slow line keeps the strongest level of the frames between) | dropdown | PRESENT | S10. spectrum_extras RowPacer Lane B. |
| 4702 | Display body: Time stamps (age, clock UTC, clock local) down the waterfall | dropdown | PRESENT | S8. clockTicks Lane B. |
| 4703 | Display body: Trace (adds Min hold to Normal, Peak hold, Average, Persistence) | dropdown | PRESENT | S5. TraceMode::MinHold Lane B. |
| 4704 | Display body: Trace shape (line, filled, bars) | dropdown | PRESENT | S6. paintTrace shape Lane B. |
| 4705 | Display body: Peak labels (off, 3, 5, 8 strongest named on the spectrum) | dropdown | PRESENT | S20. findPeaks; the labels are marks Lane B. |
| 4706 | Display body: Band zoom (full span, bands over the baseband, the tuned frequency's surroundings) | dropdown | PRESENT | S25. Lane B. |
| 4707 | Display body: SNR readout (box on the spectrum) | checkbox | PRESENT | S14. measureSnr Lane B. |
| 4708 | Display body: Channel power (box on the spectrum; between markers A and B when both are set) | checkbox | PRESENT | Q2. channelPowerDb Lane B. |
| 4709 | Display body: Frequency tracker (box on the spectrum) | checkbox | PRESENT | Q3. FreqTracker Lane B. |
| 4710 | Display body: Signal history graph, and Save signal history (CSV) | checkbox | PRESENT | S15. SignalHistory, saveText Lane B. |
| 4711 | Display body: reference trace - Save the trace as the reference, Show, Clear | key | PRESENT | S22. ReferenceTrace Lane B. |
| 4712 | Display body: Compact window (forces the compact strip and bar) | checkbox | PRESENT | S26. MainWindow.compact-forced Lane B. |
| 4713 | Spectrum and waterfall right-click menus: Set marker A here / Set marker B here / Clear markers A and B (B's name carries the differences) | menu | PRESENT | S21. AbMarkers Lane B. |
| 4714 | Spectrum: a bookmark's name is a key (a click tunes there) | key | PRESENT | S23. SpectrumEvents.bookmark-clicked Lane B. |
| 4715 | Spectrum: two-finger pinch zooms about the fingers | behaviour | PRESENT | S27. ScaleRotateGestureHandler; scripted only (no touch screen) Lane B. |
| 4716 | Demod scope: WATERFALL key (the audio spectrum over time) | key | PRESENT | S11. scope_analysis Lane B. |
| 4717 | Demod scope: AUTOCORR key (autocorrelation of the audio) | key | PRESENT | S17. Lane B. |
| 4718 | Demod scope: CEPSTRUM key (cepstrum of the audio, pitch) | key | PRESENT | S17. Lane B. |
| 4719 | Demod scope: EYE key (eye diagram of the baseband I) | key | PRESENT | S18. Lane B. |
