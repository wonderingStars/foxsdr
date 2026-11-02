# Slint port, phase 1: the inventory walked row by row

Every row of `inventory/main-window-frame.txt` and `inventory/spectrum-waterfall.txt` (FoxSDR 0.99.104, the design's source of truth) against the Slint main screen (`src/slint/ui/main.slint`, `src/slint/main.cpp`). **PRESENT** = built and wired; **RELOCATED** = moved as the Workbench IA says (a relocation, not a loss); **DEFERRED** = not in phase 1, with the reason. Companion screens, the plugin kit and the 3D wells are phases 3+. Updated with each phase.


## Main window frame — Window layout (how the main window is built)

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 1 | Cabinet | Brass frame, bevel, four screws, sunk well around everything | RELOCATED | no cabinet on Workbench: the window ground, hairlines and panels replace the brass frame (BRIEF: restyle allowed) |
| 2 | Title rail | Top strip of the cabinet; replaces the OS title bar (Windows only) | PRESENT | the 36 px custom title bar on every platform (main.slint title-bar) |
| 3 | Deck (toolbar) | Directly under the rail, full width | RELOCATED | the 104 px receiver strip |
| 4 | FUNCTION SELECT rail | Left column under the deck | RELOCATED | the 64 px icon rail + the 296 px sections panel |
| 5 | Centre | Between rail and status column: spectrum, waterfall, pages (, aw.cpp:13463) - inventoried elsewhere. Replaced by the Patch canvas when the PATCH view key is pressed (, aw.cpp:4761) | PRESENT | tab row + spectrum + splitter + waterfall; PATCH and SCOPE tabs show the phase-3 stubs |
| 6 | STATUS column | Right column under the deck | RELOCATED | the 28 px status bar of seven chips |
| 7 | Scope mode | Replaces rail + centre + status column; the deck stays. Entered from a radar/instrument;  leaves it when nothing is being typed and no popup is open (, aw.cpp:20136, Esc at ~20160) - inventoried elsewhere | PRESENT | the SCOPE tab; the instrument face is a phase-3 stub |
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
| 61 | Bank key SIGNAL | key (latched toggle, radio group; lamp strip beneath) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5 |
| 62 | Bank key DECODE | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5 |
| 63 | Bank key TOOLS | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5 |
| 64 | Bank key EXTEND | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5 |
| 65 | Bank key SYSTEM | key (latched, radio group) | PRESENT | RailKey with icon, caption and F-key hint; F1..F5 |
| 66 | Bank group caption | readout | PRESENT | the panel's head caption |
| 67 | Bank curtain | readout (animation) | DEFERRED | the enamel fade is the ImGui window's; the panel width animates on fold instead (cosmetic) |
| 68 | View key RECEIVER | key (latched, 2-way) | RELOCATED | the RECEIVER tab of the tab row (Workbench IA 4) |
| 69 | View key PATCH | key (latched, 2-way) | RELOCATED | the PATCH tab (the canvas is the phase-3 stub) |
| 70 | Sections area | list | PRESENT | the bank's rows with state chips in a Flickable; the Radio body is drawn (passband, tune step, band-plan checkbox), the other bodies are the banks of phase 3 |
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
| 120 | Ctrl+F2 | Start / stop the receiver (startStop) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 121 | M | Mute the sound (mute) - toggles user mute; lights the MUTE lamp | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 122 | + (the  key) / - | Volume up / down, 0.05 per press (volumeUp, volumeDown) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 123 | Ctrl+Shift+PageUp / PageDown | Squelch up / down, 2 dB per press, switches AUTO squelch off (squelchUp, squelchDown) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 124 | Ctrl+F | Mode: NFM (narrow FM) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 125 | Ctrl+Shift+F | Mode: WFM (broadcast FM) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 126 | Ctrl+A | Mode: AM | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 127 | Ctrl+D | Mode: DSB | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 128 | Ctrl+U | Mode: USB | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 129 | Ctrl+C | Mode: CW | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 130 | Ctrl+L | Mode: LSB | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 131 | Ctrl+R | Mode: RAW | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 132 | Ctrl+Up / Ctrl+Down | Tune up / down one step (the 10 kHz digit, cell 5) (tuneStepUp, tuneStepDown) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 133 | Ctrl+PageUp / Ctrl+PageDown | Tune up / down one screen width (visible span) (tuneSpanUp, tuneSpanDown) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 134 | T | Type a frequency - opens the counter's editor (quickTune) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 135 | Ctrl++ / Ctrl+- | Zoom the spectrum in / out about the centre (zoomIn, zoomOut) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 136 | Ctrl+Delete | Zoom back to the full span (zoomReset) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 137 | Shift+R | Record the audio - starts/stops the audio recording (record) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 138 | Space | Transmit (hold) (transmitPtt) | DEFERRED | the TRANSMIT page is a companion screen (phase 3) |
| 139 | Ctrl+W | Save a screenshot (screenshot) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 140 | F11 | Maximise / restore the window (fullscreen) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 141 | F1 / F2 / F3 / F4 / F5 | Rail banks SIGNAL PATH / DECODE / TOOLS / EXTEND / SYSTEM (bankSignal, bankDecode, bankView [sic, id kept for TOOLS], bankExtend, bankSystem) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 142 | F7 | Settings and key bindings - goes to SYSTEM and opens the Key bindings row (openSettings) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 143 | F12 | Always saves a screenshot (hard-wired, not in the table) | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 144 | Esc | Cancels the counter editor, the key-capture, a dialog; leaves Scope mode when nothing is being edited and no popup is open | PRESENT | the root FocusScope dispatches the default binding by config id; the host acts |
| 145 | Ctrl+= / Ctrl+- / Ctrl+0 | Interface size: step up / step down through 100-250%, Ctrl+0 back to Auto. Not rebindable; ignored while text is being typed | DEFERRED | the interface size (window scale) is phase 2; the chords are bound and log their request |
| 146 | Mouse wheel over a counter figure, a dial | Tunes that digit / turns the dial | PRESENT | wheel over a figure steps it; wheel over a slider moves it 5 % |

## Main window frame — Themes and sizes

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 147 | Today's bench () | The default brass-and-enamel bench; serif ("bench-serif") fonts, Nixie counter | DEFERRED | the ImGui themes are not ported: Workbench is the Slint filling and the canvas skins are the owner's choice (one struct, one assignment) |
| 148 | Bench Classic XL () | Today's palette, counter figures doubled, switches put away, every reading 1.4x | DEFERRED | the ImGui themes are not ported: Workbench is the Slint filling and the canvas skins are the owner's choice (one struct, one assignment) |
| 149 | Night Watch () | Dark red-black palette with red sheen/highlights (preserves night vision); mono fonts, flat keys | DEFERRED | the ImGui themes are not ported: Workbench is the Slint filling and the canvas skins are the owner's choice (one struct, one assignment) |
| 150 | Glass Cockpit () | Dark blue-grey; sans UI font, condensed captions, chrome knobs, radius 8 | DEFERRED | the ImGui themes are not ported: Workbench is the Slint filling and the canvas skins are the owner's choice (one struct, one assignment) |
| 151 | Daylight Lab () | Light grey/white; sans fonts, chrome knobs, radius 6 | DEFERRED | the ImGui themes are not ported: Workbench is the Slint filling and the canvas skins are the owner's choice (one struct, one assignment) |
| 152 | Field Radio () | Olive/khaki; mono body with stencil captions | DEFERRED | the ImGui themes are not ported: Workbench is the Slint filling and the canvas skins are the owner's choice (one struct, one assignment) |

## Main window frame — Themes and sizes: the Display section's settings

| # | Control | Type | Status | Note |
|---|---|---|---|---|
| 153 | Interface size | dropdown "Interface size" | DEFERRED | the SYSTEM > Display body is a phase-3 bank; the chords are bound |
| 154 | Theme | dropdown "Theme" | DEFERRED | the SYSTEM > Display body is a phase-3 bank |
| 155 | Frequency display | dropdown "Frequency display" | PRESENT | the counter menu's Counter face |
| 156 | Counter figures | dropdown "Counter figures" | DEFERRED | the SYSTEM > Display body is a phase-3 bank |
| 157 | Tuner switches | checkbox "Tuner switches" | PRESENT | the counter menu's Show tuner switches |
| 158 | Enlarge every reading | checkbox | DEFERRED | the counter scale is the interface-size setting of phase 2; the items are in the menu and log their request |
| 159 | Min dB / Max dB | slider x2 | PRESENT | the Display block's Floor / Ceiling sliders in the trace and marker menus |
| 160 | Aircraft icons | slider | DEFERRED | the SYSTEM > Display body is a phase-3 bank |
| 161 | Trail width | slider () | DEFERRED | the SYSTEM > Display body is a phase-3 bank |
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

## Counts

| Status | Rows |
|---|---|
| PRESENT | 189 |
| RELOCATED | 20 |
| DEFERRED | 66 |
| TODO (unclassified) | 0 |
| Total | 275 |
