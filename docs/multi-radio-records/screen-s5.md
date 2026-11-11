# Multi-radio screen, stage S5: Link tuning, Start together, keyboard selection, the saved layout, and the layout repairs S4 left (2026-11-11)

Branch `mr/screen-s5`, cut at fe2e5917 (the tip of screen S4, which contains the engine with eight live radios). Design: `docs/multi-radio-screen-design.md` (read-only copy in the engine-stage4 worktree), section 4 row S5 and 3.4, 3.5 items 4, 6 and 8. NOT in this stage: the Runs-on picker (it needs the engine's plugin stage) and the per-radio GAIN / ANTENNA / SAMPLE_RATE / DECODER lists.

| Commit | What |
|---|---|
| f37ec49c | host logic with made-up readings: Link (`link_logic`), the saved layout (`workspace_prefs`), the page keys (`Workspace::stepPage`), ten key-table rows; test_slint_radio_link_host, the key rows in test_slint_host_keydispatch and test_slint_host_system |
| b837a953 | Slint: the workspace bar wraps (layout keys, page keys, Link tuning, Start together, in a compact window the radios' selector, STOP keys and lamps, on every tab), the pane's link key; test_slint_radio_bar, specimen |
| 4e19b3e2 | host wiring: Link, Start together, the chords, the saved layout; the drive language (`key` chords, `radio range`); thirteen drive scripts on live radios; ledger rows 4231-4239 |
| 74cc01e5 | 15 strings in the 33 catalogues (six prepared, nine translated by this lane), `lang_assets.hpp` regenerated |
| 170e364a | the contrast fix (Night Watch 3.94 : 1), the status bar's overflow chip, the extra tests |
| 2dadd432 | two defects the window gate found: a `std::string` through a C vararg (the host crashed at start with a clashing saved key table) and a null model in the `pagekeys` read-back |
| (this record) | docs: multi-radio record, screen stage S5 |

## What the user can do now, and still cannot

With ONE radio the window is S4's: the one new thing it ever showed is the status bar's "Add a radio" key. Nothing of this stage is instantiated while one radio is open (the bar, the pane's link key, the offsets, the chords' targets, the overflow chip); no existing drive script was edited. Existing tests that changed, none weakened: the key-table counts (32 to 42 rows, 31 of 32 to 41 of 42 chords answered), `test_slint_radio_strip`'s flat-row check (it now runs over the bar; its 420 px bound is 480), `test_slint_host_input`'s fake target (one more parameter), `test_slint_radio_workspace` (extended).

With several radios, in both skins:
- LINK TUNING (the bar's key, off by default): linked radios move together and keep their gaps. Every open radio joins when it goes on; each pane then carries a small `Link` key that takes its radio out of the group or puts it back; a radio added while Link is on joins; a removed radio leaves. A radio that cannot follow (a tuning limit) stays where its device leaves it and its pane says so.
- START TOGETHER (the bar's key): one command, RUN to every open radio; live only while some radio is stopped.
- KEYBOARD: Ctrl+1 to Ctrl+8 select a radio (the page that holds it is shown), Alt+PageUp / Alt+PageDown turn the page of radios. They are ordinary rows of the key table (listed in the keys page under "Radio", rebindable, saved).
- SAVED LAYOUT: the layout (One / Side by side / 2 x 2), the page, the selected radio, each radio's splitter, Link on or off and which radios were taken out of the group are saved when the user changes them and come back at the next start.
- THE BAR WRAPS: nothing is clipped down to the smallest window (624 x 400), and in a window narrower than 1760 px the radios' row (selector, STOP Rn, STOP ALL / START ALL, MASTER lamps) is over the PATCH, SCOPE and plugin tabs as well.

CANNOT YET: the Runs-on picker (engine plugin stage); a gain, antenna or sample-rate list for a radio other than the first (the S4 trap stays: with radio 2 selected the strip's GAIN slider and the SIGNAL bank's Source body show radio 1's lists, a gain set there goes to radio 2 with radio 1's stage indices; left as it is, to be fixed by the lane that follows the engine merge); radios 2 to 8 do not come back after a restart until the engine's stage 5 saves them (the saved layout then waits, see below); a tuning range is not known to the host, so Link's limit handling is proved with a seam, not with a device.

## How Link behaves

- OBSERVED, NOT INTERCEPTED. Each tick the host reads every open radio's tuned frequency (it already did: the deck's states). A move of one linked radio since the last tick is the group's move; every other linked radio is asked for its frequency plus the same amount (`SET_FREQUENCY` aimed at that radio). So every way of tuning counts (a click in a pane, the strip's counter, the keyboard, a bookmark, another window), and the gap is the engine's own after its snapping. A follower's own arrival is not taken for a move (it is settled when within 500 Hz of what it was asked, or after 30 ticks). `SET_FREQUENCY` is the counter's rule: the VFO offset kept, the centre follows; and the engine's band-follow rule applies to a follower that lands in another band of the plan (it takes that band's mode), as for any retune.
- GAPS ARE THE INTENDED OFFSETS. The group keeps, for each linked radio, the frequency the group wants of it; a radio is asked for that, so a radio held at a limit does not lose its gap: when the group moves back the radio comes back with the gap it had.
- AT A RANGE LIMIT. The API gives no tuning range, so the host asks the radio for the shifted frequency and the engine does what the device does: it clamps to its edge or refuses and the radio stays where it was. The host reads the radio back; if it is not where the group wants it (within 500 Hz) after the follower's 30 ticks (a refusal) or at once (a range a test gave), the radio is marked linked-but-could-not-follow: its pane's key turns to the danger ink with an amber dot and its tip says "Radio n could not follow the link and stays at <frequency>."; the other radios move; nothing else is disturbed. With the product there is no range to clamp to, so what "stays at its limit" means is the device's: a clamping driver ends at its edge, a refusing one ends where it was (UNVERIFIED with hardware; the drive script `link-limit` uses `radio range 2 99900000 100100000`, a seam that clamps the request as a clamping device would). Proof (live generator radios, WFM, the tuneStepUp key steps 12.5 kHz, the engine's own raster): radio 1 at 100.300 MHz, radio 2 at 100.000 MHz with a range up to 100.100 MHz; after 8 steps radio 2 is at 100.100 (the gap kept, limit reached exactly); the 9th step takes radio 1 to 100.4125 and radio 2 stays at 100.100 with `workspace.link.2 == 2` and the group wanting 100.1125; the 10th likewise; 10 steps back down bring radio 1 to 100.300 and radio 2 to 100.000, `link.2 == 1`.
- WHEN A LINKED RADIO IS REMOVED. It leaves the group (and its "left out" mark is forgotten, so the next radio of that number is linked); the others are not disturbed and stay a group; with one radio left nothing is linked to anything. Link stays on: a radio added later joins at once, with the gap it has (drive `link-remove`: three radios, remove 2, +1 step moves radio 3 only, remove 3, add 2 again, +1 step moves it). A radio removed by another session (nobody tells the group) is handled by the open mask: the new radio of that number takes a new baseline (unit test).
- A RADIO TAKEN OUT BY ITS PANE KEY is never moved and does not move the others; put back, it takes the gap it has at that moment.
- ONE RADIO: Link on with one radio does nothing and is invisible (the bar is not drawn); it is still saved.

## The chords, and the proof they were free

Chosen: Ctrl+1 to Ctrl+8 (select radio 1 to 8), Alt+PageUp (previous page), Alt+PageDown (next page): the brief's, all free. Ctrl+PageUp / Ctrl+PageDown stay "tune one screen width" (checked live: `keys-radio` presses ctrl+pagedown and the selected radio retunes, no page turns).
Proof, from reading the whole key table and every key handler, then by test:
- The saved key table's 32 stock rows (`system_logic.cpp kActions`, held equal to the ImGui table by `test_slint_host_system`) hold no digit with Ctrl and no Alt chord at all; the stock table has no clash (`clashCount == 0`, asserted in `test_slint_host_keydispatch`) and the ten new stock chords are compared against all 32 old ones (320 checks).
- The fixed shortcuts of `key_dispatch.cpp` are Ctrl+K (palette), Ctrl+0 (interface size Auto), F12 and Esc: asserted to answer as before while Ctrl+1 to Ctrl+8, Ctrl+9 and Alt+1 answer what the table says (nothing for Ctrl+9 and Alt+1).
- Every `key-pressed` handler of `ui/*.slint` was read (digits.slint, fields, fitted, forms, instruments, keys, palette, patch_page, scene3d, signal_bank, sliders, system_bank, transmit_page, and the window's root FocusScope): none consumes a modified digit or Alt+PageUp / Alt+PageDown. Three accept more by design: the patch page's modal scope (`patch_page.slint:2259`) accepts every key while a patch modal is open, so no chord fires inside it; the keys page's capture accepts every key while it listens (that is how a chord is rebound onto Ctrl+1); the transmit page reads chords without consuming them (the PTT key). The scene3d's `0` / `Home` reset ignores modifiers (Ctrl+0 is the window's own shortcut, not this stage's). The ImGui window's table has no row on any of the ten chords (the saved file of a build that does not know the new rows loads: unknown ids are dropped one by one, asserted).
- Each chord is pressed through the scripted input as the window's focus scope receives it (`keys-radio`, `link-follow`): ctrl+1 .. ctrl+8 select (a radio that is not open is left alone: ctrl+8 with radio 8 removed), alt+pagedown / alt+pageup turn the page, clamped; in Side by side a page is a pair (four pages); where One is shown they select the neighbouring radio.
The rows are `selectRadio1` to `selectRadio8` and `radioPagePrev` / `radioPageNext`, appended after the 32 ImGui rows (the first 32 are still compared with `gui/key_bindings.cpp`; the ImGui window does not list or answer the new ones). A numbered row's name is the translated "Select Radio %d" formatted with its number, so the eight rows cost one catalogue key.

## Start together, in milliseconds

"Together" is ONE command, `RUN 1` aimed at `FOXAPI_TARGET_ALL_RADIOS` (the engine's own Start-together target), applied by the engine in one pass. Measured in a bounded run (`state workspace.together`, the host spin-reads each started radio's spectrum and takes the first frame newer than before the command, stamped with the ENGINE'S clock, `FoxSpectrumInfo.unixMs`, 1 ms resolution): in 11 presses on 2, 3 and 4 live generator radios (and 1 on eight, from `bar-small-1280`) the spread between the first and the last radio's first frame was 0 ms on the engine's clock every time; the first frames reached the host 22.7 to 31.7 ms after the command in 10 presses and 316.3 ms in one (all radios together in that one too). That 0 ms is the engine's resolution, not a claim of simultaneity below 1 ms: the engine publishes the radios' frames in one pass. UNVERIFIED with real devices: opening a USB tuner and its first frame are the hardware's time; the command is the same one.

## The saved layout

WHERE: in the interface half of the settings, in the setting the 3D views already use (`AppConfig::view3d`, a text of `name:body;name:body`), as one entry `workspace:lay=2,pg=1,sel=3,lk=1,ex=4,sp=45.45.45.45.45.45.45.45` (layout 0..2, page, selected radio, Link, the radios taken out of the group as a bitmask, each splitter in percent). `view3d_prefs.cpp` keeps that entry VERBATIM when a 3D page saves its own view, and the entry never disturbs the views (unit tests both ways, and `saved-restore` seeds a radar entry beside it). No key was added to the engine's half of the settings and no field to `AppConfig` (no `src/core` file was edited except `lang_assets.hpp`, generated).
WHEN: restored at start (the layout, the splitters, Link and the radios left out at once; the selection and page when the saved radio is open, within the first ten seconds, once, and never after the user has chosen a radio himself); written 15 ticks after the last change, ONLY what the user changed (`WorkspaceMemory`: the layout, a splitter, Link, a selection or page he made, a selection a removal he asked for moved). A layout the user chooses re-pages the saved selection for that layout.
DEGRADE QUIETLY AND KEEP: until the engine saves radios 2 to 8, a restart comes back with one radio. The screen then shows One and radio 1 and does NOT write the record. Proof, live (`saved-degrade`, seeded with 2 x 2 on page 2, radio 6, Link on with radio 4 left out, splitters 30 % and 62 %): with one radio `workspace.layout == 2` (chosen) and `workspace.shown == 0`, selected 1; 60 ticks later the record is the same text in the screen's memory, in the interface half and in the engine's table (`fact.config.interface`); then radios 2 to 6 are added inside ten seconds and the saved layout applies: 2 x 2 shown, radio 6 selected on page 2 (`cells == "5,6,0,0"`), `linked == "1,2,3,5,6"`, radio 4 out, the splitters where they were, and the record is still the same text. A naive "save what is shown" mutation turns it red (below). ROUND TRIP: `saved-roundtrip` makes four radios, chooses Side by side, turns Link on, takes radio 2 out, selects radio 3 with Ctrl+3, and the engine's table then holds exactly the text `saved-restore` is seeded with (nothing is written before the first change); `saved-restore` brings it all back when the radios return.
The restore window is the first 300 ticks (about ten seconds at 33 ms): a radio that returns later does not pull the selection away from where the user is.

## The layout repairs

| What S4 left | What it is now | Smallest window verified |
|---|---|---|
| at 800 px the selector, STOP keys and lamps clipped out of the bar | the bar WRAPS: its groups are placed by their own widths and a group that does not fit the rest of the row starts the next; the height is a whole number of 30 px rows and the views under it start below it (`WorkspaceModel.bar-h`) | every control of the radios and of the bar operated with a POINTER click at its centre (`press`, refused when outside the window) and its effect read back, at 800 x 560 (3 radios, `bar-small-800`), 1280 x 720 (8 radios, `bar-small-1280`) and 624 x 400 (8 radios, `bar-min-624`) |
| at 1280 x 720 with six to eight radios the last MASTER lamps clipped | the lamps are on a second row (two rows at 920 to 1100 px of centre, in both skins); the sweep asserts at most two rows from 920 px up | `bar-small-1280`: Radio 8, 7 and 6 master, the page keys, Link tuning, Start together, STOP ALL all pressed and read back, both skins |
| in compact windows the radios' row missing on PATCH and SCOPE | in a window under 1760 px the bar (radios' groups alone) is over every tab; PATCH, SCOPE and plugin views are wrapped in an offset rectangle (`ws-bar-h`, 0 with one radio) | `bar-tabs` (1280 x 720, 3 radios): on SCOPE and on PATCH a MASTER lamp, the selector, STOP Rn and STOP ALL pressed and read back, both skins |
| (found in the 624 px shot) eight page keys "Radios 1-1" ... "Radios 8-8" in layout One, wider than the bar | page keys are offered only while a multi-pane layout is on screen (they were redundant with the selector, the lamps and Ctrl+1..8 where One is shown) | `bar-min-624`, `bar-small-800`: `state workspace.pagekeys == 0`; `bar-small-1280`: 2 |

THE SMALLEST WINDOW VERIFIED is 624 x 400 (the resize handler's minimum) with the sections panel open, which leaves a centre column of 264 px: every control is reachable (a pointer click on each, effect read back, 39 steps), the bar is five rows (150 px) in Workbench, six in Night Watch (wider mono face), and leaves about 50 px of the receiver view: reachable, not pleasant; folding the panel gives the view its width back. Bar heights by width (8 radios, RECEIVER tab, compact window; Workbench / Night Watch; from `test_slint_radio_bar`): 264 px 5 / 6 rows, 440 px (an 800 px window) 3 / 4, 640 px 2 / 3, 760 to 1100 px (1280 x 720 is 920) 2 / 2, 1400 px and up 1; on another tab (the radios' groups alone) 3 rows at 264 px, 2 at 320 to 440 px, 1 from 560 px; wide windows (1760 px and up, the block is in the strip, the bar holds the layout keys, Link and Start together) 1 row from 760 px (640 px in Workbench). Wordy labels (a 30-character Link key and Start together) at 264 px wrap to 8 rows instead of being cut.

## Strings

15 keys added to each of the 33 catalogues (495 lines), `src/core/lang_assets.hpp` regenerated with `tools/embed-lang.py`, only keys the code uses; `test_i18n` passes.
FROM D:/foxsdr-build/prep/new-strings-34-languages.json (6): Link tuning; Linked radios move together by the same amount and keep the gaps between them.; Start together; Link Radio %d; Linked: moves with the other linked radios; Not linked: Link tuning never moves this radio. (Existing words reused: Link, Radio, Select Radio %d, Start every open radio, Layout, One, Side by side, Radios %d-%d, Show radios %d to %d, STOP ALL / START ALL, MASTER.) Prepared strings not used: the keyboard help lines (their chords were wrong: the design said Ctrl+PageUp / PageDown), "Select Radio %d (Ctrl+%d)" (a rebindable chord must not be written into a tip; the tip is the existing sentence), the Runs-on strings, Focus + 3, the "In use by" refusal words (the engine words its own).
TRANSLATED BY THIS LANE (9; "machine translated, not checked by a native speaker"; each written per language from the catalogue's own words for Radio, Remove, Resize, device and the neighbouring sentences, e.g. German keeps "SDR" as the catalogue does, Hungarian and Turkish take their numbered forms "%d. rádió" / "%d. radyo"): `Resize Radio %d`, `Remove Radio %d`, `Remove Radio %d?`, `This radio is running. Removing it stops it and lets go of its device.`, `This radio is recording. Removing it ends the recording and lets go of its device.`, `No other radio found.` (the six S3 and S4 left English only; their code now goes through `tr()` with `%d`), `Radio %d could not follow the link and stays at %s.`, `Previous page of radios`, `Next page of radios`. They are in `resources/lang/*.json` (commit 74cc01e5). Strings with numbers are worded in C++ with `tr()` and `%d` / `%s`.

## Theme parity: contrast of every text new in S3, S4 and S5

Measured by `test_slint_radio_bar` from each skin's own tokens (WCAG ratio; at least 4.5 : 1 asserted for every text that is read; the disabled key is exempt).

| Text on its background | Workbench | Night Watch |
|---|---|---|
| key text (Link tuning, Start together) on key | 11.73 : 1 | 7.75 : 1 |
| lit key text (Link tuning on, Link in a pane) | 8.97 : 1 | 6.98 : 1 |
| lit key text, hovered | 11.81 : 1 | 8.59 : 1 |
| danger key text (Link could not follow, STOP Rn) | 11.37 : 1 | 12.56 : 1 |
| layout / page / selector key text (SegKey) on its group | 10.69 : 1 | 6.76 : 1 |
| layout / page / selector key text, hovered | 7.11 : 1 | 5.64 : 1 |
| small-window notice on the bar | 10.71 : 1 | 9.55 : 1 |
| caption (Layout) on the bar | 6.68 : 1 | 5.03 : 1 |
| MASTER lamp number, stopped, on the bar | 6.68 : 1 | 5.03 : 1 |
| MASTER lamp number, running, on the bar | 13.36 : 1 | 14.44 : 1 |
| MASTER lamp number, failed, on the bar | 11.75 : 1 | 17.86 : 1 |
| R-tag ink on radio 1's colour (pane, panel, mix) | 6.83 : 1 | 7.25 : 1 |
| R-tag ink on radio 2's colour | 8.79 : 1 | 14.94 : 1 |
| R-tag ink on radio 3's colour | 10.06 : 1 | 4.70 : 1 |
| R-tag ink on radio 4's colour | 6.95 : 1 | 9.95 : 1 |
| Add-a-radio card: title on the card | 16.42 : 1 | 9.11 : 1 |
| Add-a-radio card: hint on the card | 7.88 : 1 | 5.31 : 1 |
| Add-a-radio card: 'No other radio found.' and the quiet cell's name (muted since stage S5) | 7.88 : 1 | 5.31 : 1 |
| Add-a-radio card: the engine's refusal on the card | 12.64 : 1 | 10.08 : 1 |
| Audio mix: the block's title on the panel | 6.61 : 1 | 4.96 : 1 |
| pane: device name on the header | 10.32 : 1 | 6.94 : 1 |
| pane: frequency on the header | 10.24 : 1 | 8.72 : 1 |
| pane: readout caption on the row | 6.68 : 1 | 5.03 : 1 |
| pane: readout value on the row | 10.80 : 1 | 6.86 : 1 |
| pane: decoder chip | 8.70 : 1 | 7.11 : 1 |
| pane: mode chip | 9.10 : 1 | 7.75 : 1 |
| disabled key text (Start together when dead) | 5.25 : 1 | 3.10 : 1 (disabled: exempt) |


One text failed and was fixed: the S4 card's "No other radio found." and the quiet cell's "Radio n" used `Theme.dim`, which is 3.94 : 1 on the card in Night Watch; both are `Theme.muted` now (5.31 : 1 Night Watch, 7.88 : 1 Workbench). No colour is hard-coded in any new part (the house-rules test passes): the new keys use the existing key tokens; the radio colours and the R-tag ink are S3's.

## Tests, and what they show

- `test_slint_radio_link_host` (168 checks, made-up readings and a made-up key table): Link off by default and then nothing moves; on, +10 kHz on one radio moves the others +10 kHz and the gaps are kept, from any radio, both directions, with no ping-pong; a radio taken out is not moved and moves nobody; put back it takes the gap it has; a known range limit (the radio stays at its limit, the others move, the intended gap is kept and comes back, upper and lower limits); an unknown limit (the follower refuses: found out at the give-up tick, the others never held back); a radio added joins; a radio removed leaves (with and without `forget`); noise below 2 Hz is not a move; two radios moved in one tick; a follower still on its way is not a mover and a second move retargets it; a radio without a state neither moves nor is moved; restore with radios not open; the words; the saved text (clamps, lenient, same text twice), sharing the 3D views' setting both ways; the record in use (degrade without writing, restore once inside the window, the window closes, a user selection ends the wait, defaults write nothing, a layout re-pages the saved selection); the page keys (clamped in a multi-pane layout, the neighbour where One is shown, Ctrl+n and a refused focus).
- `test_slint_host_keydispatch` (485): the ten rows free, answering their ids, `Ctrl+PageUp/PageDown` still the tune keys, rebinding and saving, unknown ids dropped, the keys page lists them. `test_slint_host_system` (836): the ImGui table compared for its 32 rows, the keys page has 8 groups and 42 rows. `test_slint_host_input`: the chord grammar and the `radio range` verb.
- `test_slint_radio_bar` (18005+ checks, both skins): the bar at widths 264 to 1760 px with 2, 5 and 8 radios in compact and wide windows and on another tab: every control the model names lies inside the bar and none overlaps another, the view under the bar starts at its height, long words wrap, four page keys fit from 920 px, Link tuning and Start together report their clicks and show their state, the contrast table. `test_slint_radio_workspace` (541): the pane's link key in both skins (absent while Link is off; lit, plain and the danger key with the amber dot told apart by pixels; a real click names its radio).
- Drive scripts on LIVE generator radios (the real `RADIO_ADD`): `link-follow`, `link-limit`, `link-remove`, `link-skins`, `together`, `keys-radio`, `saved-degrade`, `saved-roundtrip`, `saved-restore`, `bar-small-800`, `bar-small-1280`, `bar-min-624`, `bar-tabs` (13, all pass; 1500 to 2600 ticks each). Ledger rows 4231 to 4239: `ledger: 2176 rows, 550 asserted, 92 driven, 1534 not drivable, 0 unclassified`. Specimen `radio_bar_specimen.slint`; COMPONENTS.md updated.

### Break-it checks (each mutation applied, the test or script run, red seen, the mutation restored)

HOST LOGIC (each mutation alone: edit, build the test, run, red seen, `git checkout` the file, rebuild, green):

| Mutation | Result |
|---|---|
| L1 followers are never asked to move | 32 red of 162 |
| L2 gaps not kept (followers go to the mover's frequency) | 27 red |
| L3 Link is on by default | 4 red |
| L4 a range limit is ignored | 8 red |
| L5 could-not-follow is never reported | 5 red |
| L6 the intended gap is lost at a limit (a held radio forgets its offset) | 7 red |
| L7 a radio that went away keeps its old tracking | 0 red at first: the test always called `forget()`; a case for a radio removed by another session was added; now 3 red of 168 |
| L8 the pane key cannot put a radio back | 2 red |
| L9 a radio the user took out is moved anyway | 5 red |
| L10 noise below 2 Hz is taken for a move | 3 red |
| P1 the degraded session writes the record back (the record is marked changed at load) | 7 red |
| P2 a saved selection never comes back | 1 red |
| P3 the restore window never closes | 1 red |
| P4 a 3D page's save drops the workspace entry (`ViewPrefs::serialize`) | 12 red |
| P5 the saved page does not follow a new layout | 3 red |
| P6 a change of Link is not recorded | 1 red |
| G1 the page keys do not select the neighbour where One is shown | 4 red |
| G2 the page keys wrap instead of stopping | 3 red |
| K1 Select Radio 3's stock chord is Ctrl+9 | 3 red of 485 (`test_slint_host_keydispatch`) |
| K2 Alt+PageDown takes the tune-span chord | 7 red |
| K3 the numbered rows do not say their number | 9 red |
| K4 the clash text is given a `std::string` where a C string is meant (the defect the window gate found) | 1 red of 486 (it was not red before the case was added) |

COMPONENTS (`test_slint_radio_bar` 18035 checks, `test_slint_radio_workspace` 541; each alone): C1 the bar's groups do not report their widths 5532 red; C2 the bar never starts a new row (the S4 clip) 595 red; C3 no radios' groups in a compact window 480 red; C4 Link tuning does not show that it is on 4 red; C5 Start together is never dead 4 red; C6 no layout keys on the receiver tab 360 red; C7 the pane has no link key: the test aborts (the control is not found); C8 the pane's key is not lit when linked 2 red; C9 the could-not-follow key has no danger ink 2 red; C10 a click on the pane key reports no radio 4 red. Restored: 0 failed.

WIRING (one build of `main.cpp` and `workspace_publish.cpp` with eleven mutations at once, all thirteen new drive scripts run, then restored; each behaviour is named by the steps it turned red): Start together aimed at the focus (W1: `together` steps 22-25, 27, 30-32, 37: only radio 1 started, `first=1`; `bar-small-1280` 35; `bar-min-624` 25); the follower's SET_FREQUENCY never sent (W2: `link-follow` 35, 36, 54, 59, 61, 69-71, 79-81; `link-limit` 17-83 (20 steps); `link-remove` 24, 39); Ctrl+n ignored (W3: `keys-radio` 19, 22-24, 54, 59, 61, 66, 68); the page keys do nothing (W4: `keys-radio` 31, 33, 35, 45, 46, 49, 50, 52); the record never written (W5: `saved-roundtrip` 23, 24 (the interface half and the engine's table)); a saved selection never applied (W6: `saved-degrade` 31-34, `saved-restore` 14-17); a naive save of what is shown overwrites the record (W7: `saved-degrade` 16, 19, 40, `saved-restore` 24); the saved layout not applied at start (W8: `saved-degrade` 8, 21, `saved-restore` 6); Link can never be turned off (W9: `link-follow` 75-77, `bar-small-800` 35, `bar-small-1280` 40, `bar-min-624` 29); the pane's link keys not shown (W11: `link-follow` 29-31, 46, 49, 63; `link-limit` 47; `link-skins` 13, 14, 34); page keys offered where One is shown (W12: `bar-min-624` 32: `pagekeys` 2, wanted 0). 107 steps failed in all (13 scripts: 3, 3, 20, 2, 17, 2, 9, 0, 1, 25, 6, 7, 12 by script); they were mutated together, so a step that fails does not say which of two mutations it was only where two touch the same step (`saved-degrade` and `saved-roundtrip` carry W5 to W8 and W3 / W11: the line ranges keep them apart). Restored and rebuilt: the thirteen scripts pass.

`main.slint` (one mutation build, 16 minutes of compile, two mutations at once): the radios' row absent on the SCOPE tab, and the PATCH view not offset below the bar. `bar-tabs` turned red at steps 12-20 and 40-41 (SCOPE: no control labelled "Radio 3 master", "STOP ALL" ...) and at 27-31 and 46 (PATCH: `press "Radio 2 master"` hit the patch page under it: `workspace.selected` stayed 1). Restored with the contrast fix and rebuilt: green.

NOT BREAK-IT-CHECKED: the periodic restore window's edge in the live window (the logic is, P3); the status bar's overflow chip (looked at in the shots; the drive asserts `workspace.chips == "0"`, which the unmutated build satisfies and an eight-chip build would not: not mutated); the saved text's 1024-character share with the 3D views (the limit is core's, the entry is about 90 characters).

## Screenshots (looked at)

`D:/foxsdr-build/mr-screen1-shots-s5/window/` (the real window on the hidden desktop, live radios), `bar/` (the bar at 264 / 440 / 920 / 1400 px, three modes, both skins, the wordy and four-page-key cases), `comp/` (the pane's link key in both skins).
What I see, and what is wrong:
1. 1600 x 900, three radios (`link-skins-*`): one bar row (Layout, Link tuning lit, Start together, R1 R2 R3, STOP R1, STOP ALL, the lamps); R1's pane `Link` lit, R2's red with an amber dot (could not follow: it stops at 100.100 MHz while the group is at 100.4125), R3's plain (left out). Night Watch: the same in its reds; the amber dot is the skin's lamp colour. Legible. Wrong, not mine: R2's device name elides to "Signal g..." (the header is crowded: S2's order says the name goes first).
2. 800 x 560 (`bar-800-*`): the bar is four rows in Night Watch, three in Workbench (selector and STOP keys and the lamps each reachable); nothing under it is covered. Wrong, not mine and not new: the tab row's right group (Trace / Range / Band plan keys) and the strip's right half are cut at this width as they are with one radio (S3 noted the tab row); a window this small shows little of the view.
3. 624 x 400 (`bar-624-*`): five rows, reachable; the spectrum is squeezed to about 50 px. Wrong, accepted: this is the minimum window; the panel's fold key helps.
4. 1280 x 720, eight radios (`bar-1280-*`): two bar rows (the lamps on the second); the 2 x 2 grid starts below; all eight lamps visible; the Audio mix at the panel's foot scrolls (S4). The Side by side shot shows pages as pairs.
5. PATCH and SCOPE tabs (`bar-tabs-*`): the radios' row at the top with the page below it, in both skins.
6. WRONG, FOUND AND FIXED here: (a) the 1280 x 720 shot with eight radios showed the status bar's last key ("Report a bug / dislike") cut off: S4's eight radio chips (a lamp and "R1" each, about 52 px) leave it no room. With more than four radios in a window under 1500 px the chips are now ONE chip, "Radios 1-8" (the catalogue's existing "Radios %d-%d"), with the worst state's lamp and one line per radio that a click opens; the MASTER lamps in the bar select (the shot after the fix: both keys whole; `bar-small-1280` asserts `state workspace.chips == "0"`). (b) the first 624 px shot showed eight page keys ("Radios 1-1" ... "Radios 8-8") cut at the bar's edge in layout One (S3's behaviour; the component sweep had only fed two labels): page keys are offered only while a multi-pane layout is on screen. And the contrast table found the Night Watch 3.94 : 1 note (above).
7. WRONG, NOT FIXED: in Night Watch the MASTER lamps' running and failed dots are close in colour (S4's note: the skin's own lamp colours; the number beside each lamp and its tip tell them apart).

## Gates

On `D:/foxsdr-build/mr-screen1`, Release. The gates were run ONE AFTER THE OTHER (headless, then the window gate on the hidden desktop `FoxSDRTestsS5`, `FOXSDR_FRAME_CAP_HZ=60`; no window was shown on the visible desktop). Another lane's window gate (`mr-dsp`) ran on this machine during mine, so both took longer than S4's.
- Build: `cmake --build D:/foxsdr-build/mr-screen1 --config Release --parallel`, exit 0 (the generated code of `main.slint` compiled three times this lane: the first S5 build, the mutation build and the final one, 14 to 19 minutes each; every other build was seconds to a few minutes).
- Headless gate, `ctest -C Release -L "headless|slint" -j4 --timeout 300` on the tree at 170e364a: 755 tests, 749 passed, 6 failed, 1286 s. The six: the five known false failures of a D: build (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`: from the worktree root they pass, 115, 5174, 5, 22 and 49 checks, 0 failed) and `test_engine_api_transmit` (known, about one run in two on every tree; it failed alone as well, 104 s). `test_diagnostics_free` passed (the build had been refreshed after the last commit of that tree). After the two fixes of 2dadd432 the `slint`-labelled tests that do not open a window (74) were run again: all pass.
- Window gate, `ctest -C Release -L gui -j3 --timeout 300` on the same tree: 257 tests (the 244 of S4 and the thirteen new drive scripts), 252 passed, 5 failed, 1831 s: `slint_drive_bar-min-624` and `slint_drive_bar-small-800` (MINE: `state workspace.pagekeys` dereferenced a model that is not set until the first page key is offered: the host exited with an access violation at that step), `test_slint_config_roundtrip` (MINE: exit 0xC0000409 at start with the seeded settings: `readKeyBindings` passed a `std::string` through a C vararg for the clash text, "also %s", and the seed holds two actions on F2; `test_slint_host_system` had passed by luck because a short `std::string`'s first bytes are its text), `test_deck_knobs_app` (a known load flake; it passes alone, 117 s) and `test_diagnostics` (the known stale commit stamp). Each of mine was re-run alone (red, the same way), fixed in 2dadd432, and re-run: the three pass; a clash case with a long name was added to `test_slint_host_keydispatch` (red without the fix, green with it). Then 48 window tests were run again on the fixed build (the thirteen new scripts, every `deck-`, `live-`, `workspace-`, `system` and `key` script, `test_slint_config_roundtrip`, `test_deck_knobs_app`, `test_slint_main_screen`): 100 % passed, 509 s. `test_diagnostics` and `test_diagnostics_free` compare the build's commit stamp with HEAD, so they fail after any commit until the build is refreshed: the build was refreshed after this record's commit and both were run again (their result is in the lane's report; writing it here would be one more commit and make them stale again).
- Ledger: `ledger: 2176 rows, 550 asserted, 92 driven, 1534 not drivable, 0 unclassified` (the check passes inside the slint label).
- New and changed tests alone: `test_slint_radio_link_host` 168 checks, `test_slint_radio_bar` 18035, `test_slint_radio_workspace` 541, `test_slint_host_keydispatch` 486, `test_slint_host_system` 836, `test_slint_host_input`, `test_i18n` pass; house rules pass.

## Deviations from the brief, and decisions made

1. THE SAVED LAYOUT IS IN `view3d`, NOT IN A NEW FIELD: the brief said to use the place the screen already keeps its own settings and not to add keys to the engine's half; the interface half's one free-form setting (`view3d`) is that place and `AppConfig` is in `src/core`, which this lane does not edit. The cost: the entry shares a text with the 3D views (1024-character limit, entries separated by `;`), and `ViewPrefs` had to learn to keep one entry verbatim (view3d_prefs.cpp, tested both ways).
2. KEY ROWS APPENDED TO THE HOST'S COPY ONLY. The ImGui window's table is not changed (it would not answer them); `kKeyActionCount` is 42, `kClassicKeyActionCount` 32 and the drift test compares the first 32.
3. NO "Select Radio %d (Ctrl+%d)" TIP: the selector key's tip stays "Select Radio %d" (a rebindable chord must not be baked into a sentence).
4. A restore window of about ten seconds for the saved selection (not forever): a radio that returns much later must not move the user's selection. The layout, the splitters and Link are applied at once.
5. LINK IS OBSERVED, NOT INTERCEPTED (above), at the cost of a one-tick lag and the engine's own snapping in the gap; chosen because it covers every way of tuning and measures the gap the engine really has.
6. PAGE KEYS ONLY WHERE A MULTI-PANE LAYOUT IS SHOWN (above).
7. `RadioStripRow` is gone (the bar's groups replace it); the S4 flat-row check of `test_slint_radio_strip` now runs over the bar.
8. THE STATUS BAR'S RADIO CHIPS COLLAPSE into one chip above four radios in a window under 1500 px (above): S4's design has one chip per open radio; it no longer fits the bar there and the brief says nothing may be clipped. A click on that chip no longer selects a radio (its lines are text); the MASTER lamps and the selector do.
9. THE TUNE STEP: the key table says "Tune up one step (10 kHz)" and the engine's STEP_TUNE moves 12.5 kHz in WFM (its raster; found when the first drive run read 100312500). The scripts use what the engine does; the label is S-pre and not touched.

## Engine gaps and defects found

- ENGINE GAP: no tuning range per radio in the API (`FoxEngineApi` / facts), so the host cannot clamp a follower to a device's limit or say why it stopped; Link's limit handling asks for the shifted frequency and reads back. Suggest `fact.radio.<n>.range` (lo, hi in Hz, absent when the device has none).
- ENGINE GAP (S4's, still open): no per-radio GAINS / ANTENNAS / SAMPLE_RATES / DECODERS lists; the lists fingerprint holds no receiver count; no solo read-back.
- ENGINE NOTE: `STEP_TUNE` moves by the mode's raster (12.5 kHz in WFM) whatever step the host sends; the key table's text says 10 kHz.
- OBSERVATION: the first frame after a Start together came 316 ms after the command once in 12 presses (22.7 to 31.7 ms otherwise); no defect shown, one slow start on a loaded machine.
- No engine defect with a reproduction was found in this stage. (The two defects the window gate found were this lane's own: a `std::string` through a C vararg in `readKeyBindings`, and a null model in a read-back; both fixed in 2dadd432.)

## What the next lane must know

- RUNS-ON PICKER: the bar and the pane header have the room (the bar wraps); the saved layout's text has room for a per-plugin radio (`workspace:` keys are free; the 1024-character limit of `view3d` is shared with the 3D views: about 90 characters used).
- PER-RADIO LISTS: the strip's GAIN slider and the SIGNAL bank still read radio 1's lists when another radio is selected (the S4 trap, deliberately left); the seam is `readRadioFacts` / the deck's per-radio reads in `main.cpp`.
- S6 SWEEPS: the bar's elide / wrap sweep in the longest languages (the component test has the wordy case at 264 px only), the new strings' clip sweeps (Link tuning, Start together, the link key, the new keys-page rows in the longest languages), `state radio <n> <field>` over the `workspace.*` read-backs, which now include `link`, `linked`, `link.<n>`, `intended.<n>`, `together`, `togetherms`, `saved`, `savedview`, `split.<n>`, `pagekeys`.
- THE DRIVE LANGUAGE: `key` takes chords; `radio range <n> <lo> <hi>` (test only); a drive script can be seeded with a saved layout through `config=<fixture>.json`.
- COMPILE COST: any change to a `.slint` file recompiles `main.slint`'s generated code (16 to 25 minutes here); keep the Slint edits of a lane few and batched. Everything in this stage's host side compiles in seconds.

## Not covered / unverified

- Hardware: no real SDR. A real tuning limit (clamp or refusal), two real radios at once, the first frame of real devices after Start together, a real device list: UNVERIFIED.
- Sound: nobody listened (Start together and Link were read back from the engine's state).
- Real mouse and keyboard: the drive scripts use the element tree, `press` pointer clicks and the focus scope's key events; a real-mouse pass is the release gate. Windows' own handling of Alt+PageUp / Alt+PageDown was not exercised (the chords arrive as Slint key events).
- Native speakers: the nine strings this lane translated and the six prepared ones are machine translations nobody read; the longest-language clip sweeps of the new parts are S6's.
- The mutation of `main.slint`'s tab placement was run once for two mutations together (below), not one by one.
- `%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` both times (every run used a scratch profile).
