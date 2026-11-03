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
| 364 | Trail style | dropdown/combo | DEFERRED | trail style: the compositor draws 2 px lines; the setting lives in the interface half (config) with the INTERFACE_CONFIG work |
| 365 | Trail width | slider | DEFERRED | trail width: the compositor draws 2 px lines; the setting lives in the interface half (config) with the INTERFACE_CONFIG work |
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

# Counts (the whole file: phase 1 and phase 2)

| Status | Rows |
|---|---|
| PRESENT | 1077 |
| RELOCATED | 54 |
| DEFERRED | 79 |
| TODO (unclassified) | 0 |
| Total | 1210 |

Phase 2 alone: PRESENT 888, RELOCATED 34, DEFERRED 13, total 935.
