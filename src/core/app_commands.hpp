// app_commands.hpp - the ONE vocabulary every change to the receiver is said in
// (engine extraction, stage 1; docs/engine-stage1.md).
//
// WHAT THIS IS. The FoxSDR engine API (third_party/foxsdr_api/foxsdr_api.h,
// API 0.3: 0.2 as vendored, extended by docs/api-0.3-additions.md) describes a change to the receiver as a
// FoxCommand: an op and a few typed slots. Stage 1 makes that literally the
// only way the application changes its receiver: a desktop widget, a key
// binding, a browser (web /api/control), CAT and a plugin (host API level 1)
// all end as FoxCommands applied by ONE function, AppWindow::applyCommand.
// This header is the half of that which needs no window: the extension ops,
// the builders every caller uses, and the op names for logs and tests.
//
// EXTENSION OPS (FOXAPP_OP_*, 0x8000-0x8FFF). API 0.2 has gaps (its own
// docs/API.md 10.7), and the desktop has controls whose exact behaviour no
// 0.2 op carries. Each such control gets an app-internal op here rather than
// a quiet change of behaviour; every one is listed, with its reason, in
// docs/engine-stage1.md so the API can adopt or retire it later. None of
// them is ever sent across a transport: the web and CAT parsers produce only
// API ops or the extension ops documented as translations of their own
// fields.
//
// THE LONG-TEXT CHANNEL. FoxCommand::text holds 255 bytes (API gap 10). A
// path typed into the desktop can be longer, so a command applied in-process
// may carry its full text beside the struct (QueuedCommand::longText); an op
// that takes text reads the long text when it is not empty. makeText() fills
// both, so no caller decides.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "foxsdr_api.h"

// --- extension ops -------------------------------------------------------------
// Tuning.
#define FOXAPP_OP_VFO_TO_ABSOLUTE        0x8101u /* num[0]: air Hz; ival[0]: 1 = snap to the mode's raster. Click-to-tune. */
#define FOXAPP_OP_SET_VFO_OFFSET_FREE    0x8102u /* num[0]: offset Hz, NOT clamped into the band (the rail's VFO slider) */
// Demodulator and audio.
#define FOXAPP_OP_SET_BANDWIDTH_STEP     0x8201u /* ival[0]: index into the bandwidth steps; not clamped (the rail combo) */
#define FOXAPP_OP_SET_BANDWIDTH_DRAG     0x8202u /* num[0]: Hz, clamped; the step index left alone (a spectrum edge drag) */
#define FOXAPP_OP_SET_NR_STRENGTH        0x8203u /* num[0]: 0..1, the enable left alone */
#define FOXAPP_OP_SET_NOTCH_FREQUENCY    0x8204u /* num[0]: Hz, the enable and Q left alone */
#define FOXAPP_OP_SET_NOTCH_Q            0x8205u /* num[0]: Q, the enable and frequency left alone */
// Display.
#define FOXAPP_OP_SET_DISPLAY_MIN        0x8301u /* num[0]: dB; dbMax kept, dbMin yields to the minimum span */
#define FOXAPP_OP_SET_DISPLAY_MAX        0x8302u /* num[0]: dB; dbMin kept, dbMax yields to the minimum span */
// Source and device.
#define FOXAPP_OP_SCAN_DEVICES_ON_OPEN   0x8401u /* the Source list opening: native always, SoapySDR if never (wholly) scanned */
#define FOXAPP_OP_SET_NETWORK_USRP_SCAN  0x8402u /* ival[0]: 0/1 "Look for network USRPs"; on rescans SoapySDR */
#define FOXAPP_OP_SET_GAIN_NO_READBACK   0x8403u /* text: stage; num[0]: dB. The radar scope's GAIN knob (no readback) */
#define FOXAPP_OP_SET_CONVERTER          0x8404u /* ival[0]: 0 off 1 up 2 down; ival[1]: inverted; num[0]: LO Hz */
#define FOXAPP_OP_SOUNDCARD_IQ_CENTRE    0x8405u /* num[0]: Hz the external receiver is tuned to; a running I/Q card follows at once */
#define FOXAPP_OP_SCAN_NATIVE_ONLY       0x8406u /* a Radio part added (0.99.40): the native walk only, never the SoapySDR probe */
#define FOXAPP_OP_PATCH_RADIO_LIST_OPENED 0x8407u /* a Radio's device list opened (0.99.40): native at once; SoapySDR a deferrable wish, first time only */
// Bookmarks and scanner.
#define FOXAPP_OP_BOOKMARK_IMPORT_FILE   0x8601u /* text (or long text): a PATH on this machine; LOCAL only */
#define FOXAPP_OP_SCANNER_RANGE          0x8602u /* ival[0]: mask 1 start 2 stop 4 step 8 reconfigure-if-running; num[0..2]: Hz */
#define FOXAPP_OP_BOOKMARK_EXPORT        0x8603u /* long text: bookmark ids, decimal, space-separated; writes them as SDR# XML under the record folder; ival[0] 1: the text is a preset's (group's) name and its rows go out as a CSV; the file is written on a worker (B19); LOCAL only */
// THE FREQUENCY LIST'S TICK, ROWS TYPED IN, AND THE SCANNER'S LIST MODE (stub campaign B6, 0.99.60 - 0.99.66; docs/api-0.3-additions.md 9.6), all LOCAL
// sessions only and none of them on the web profile. A bookmark's tick is what the scanner's list mode and the Airband monitor play.
//   BOOKMARK_TICK (SETTINGS): ival[0]: the bookmark's id; ival[1]: 0 untick, 1 tick. NOT_FOUND for an id the list does not hold. A row already
//   as asked changes nothing (and writes nothing).
//   BOOKMARK_ADD_ROWS (SETTINGS): long text (or text), one row a line: "<Hz>\t<mode>\t<bandwidth Hz>\t<flags>\t<group>\t<name>" (the group and the
//   name are the last two fields and may be empty; a name with a tab in it is the sender's to clean; <flags> is bit 0 ticked, bit 1 favourite - "0" and
//   "1" as every sender wrote them before the favourite was carried, stub campaign B19). ival[0]: 0 a row TYPED IN - added as the list's
//   own add does (a name already in use gets " (2)"), and judged by the rule a tune the radio refuses is: a frequency outside what the open radio
//   covers (a converter's LO taken out) is OUT_OF_RANGE with the sentence that says what it does cover - 1 a LIST (an airport's rows) - added as an
//   import does, a row identical in name, frequency and group to one already there skipped, no range judged. applied[0]: the rows added. BAD_ARGUMENT
//   for text with no row in it.
//   SCANNER_LIST (TUNE): ival[0]: 0 range mode (the scanner scans the range again), 1 list mode (it scans the ticked rows, in frequency order, each
//   tuned with its own mode and bandwidth) - either stops a running scan, as the checkbox did - 2 START a list scan (list mode, whatever it was).
//   A start with nothing ticked is NO_CHANGE. List mode is session-only. While a list scan runs it follows the ticks: a row ticked or unticked
//   restarts it from the first row, nothing ticked stops it. SCANNER_RUN (a range scan, from the desk or a browser) leaves list mode.
#define FOXAPP_OP_BOOKMARK_TICK          0x8604u /* ival[0]: the bookmark's id; ival[1]: 0 untick / 1 tick */
#define FOXAPP_OP_BOOKMARK_ADD_ROWS      0x8605u /* long text: "<Hz>\t<mode>\t<bw Hz>\t<flags: 1 tick, 2 favourite>\t<group>\t<name>" lines; ival[0]: 0 typed rows / 1 a list */
#define FOXAPP_OP_SCANNER_LIST           0x860Au /* ival[0]: 0 range mode / 1 list mode / 2 start a list scan */
// THE WATERFALL'S MARKERS (stub campaign B10, 0.99.57; docs/api-0.3-additions.md 9.8), all LOCAL sessions only and none of them on the web profile. A marker is
// a pencil tick made while looking at something else - a frequency, a number to call it by, the moment it was made and a note added later - and NOT a bookmark;
// the list is the engine's (Engine::freqMarkers_, markers.json, saved on a worker) and is read as the list FREQ_MARKERS (53). THE NUMBERS ARE NAMES: M1, M2, ... are
// handed out in the order markers are dropped and never again while the list lives - only CLEAR starts the count at M1 again - and the count is persisted.
//   MARKER_DROP (SETTINGS): num[0]: the frequency, ABSOLUTE Hz (the pointer's, on a patch Display part the part's radio's centre taken in: never the part's
//   offset); num[1]: the merge reach, Hz (0: none) - a marker within it of one already there is THAT marker and nothing is added, so a second right-click
//   on the same signal does not stack two flags on one pixel. applied[0]: the marker's number; applied[1]: 1 a new marker, 0 the one already there. The
//   engine stamps the time. OUT_OF_RANGE for a frequency that is not positive, LIMIT when the list holds its 200 (or the numbers are spent).
//   MARKER_REMOVE (SETTINGS): ival[0]: the marker's number. NOT_FOUND for a number the list does not hold. The number is not handed out again.
//   MARKER_CLEAR (SETTINGS): no arguments. Empties the list and starts the numbering again at M1. NO_CHANGE for a list that is empty already.
//   MARKER_NOTE (SETTINGS): ival[0]: the marker's number; text (or long text): the note, as typed ("" clears it). NOT_FOUND for a number the list does not
//   hold; the same note again changes nothing and writes nothing.
#define FOXAPP_OP_MARKER_DROP            0x8606u /* num[0]: absolute Hz; num[1]: merge reach Hz (0 none); applied[0]: the number, applied[1]: 1 new / 0 the one already there */
#define FOXAPP_OP_MARKER_REMOVE          0x8607u /* ival[0]: the marker's number */
#define FOXAPP_OP_MARKER_CLEAR           0x8608u /* no arguments: empties the list, the numbering starts again at M1 */
#define FOXAPP_OP_MARKER_NOTE            0x8609u /* ival[0]: the marker's number; text (or long text): its note */
// Plugins and the store.
#define FOXAPP_OP_USER_PRESET_APPLY      0x8701u /* text: plugin key; ival[0]: ordinal among that plugin's user presets */
#define FOXAPP_OP_USER_PRESET_FORGET_AT  0x8702u /* text: plugin key; ival[0]: ordinal among that plugin's user presets */
#define FOXAPP_OP_PLUGIN_AUTO_PRESET     0x8703u /* text: plugin key; ival[0]: 0 it was started, 1 its window was opened */
#define FOXAPP_OP_DECODER_STOP_LIST      0x8704u /* text (or long text): plugin keys, one per line; ival[0]: 1 = and end the mute they caused */
#define FOXAPP_OP_STORE_UPDATE           0x8705u /* text: catalogue id of an installed plugin to update */
#define FOXAPP_OP_STORE_REMOVE_BLOCKED   0x8706u /* text: file name of a plugin the catalogue policy blocked */
// HOW A FITTED PLUGIN RUNS (stub campaign B2, 0.99.73; docs/api-0.3-additions.md 9.3), both LOCAL sessions only.
//   PLUGIN_RUN_MODE (SETTINGS): text: the plugin's module file name (or its id); ival[0]: 0 AUTO (runs while something uses it), 1 ALWAYS
//   (pinned: "keep running"), 2 STOPPED. NOT_FOUND for a name no installed plugin carries and no run state records; OUT_OF_RANGE for any other
//   ival[0]. Pins or stops that ONE plugin's instances; nothing else is rebuilt.
//   PLUGIN_USE (VIEW): long text (or text), the window's whole table of what it shows, replacing the last: one line "<file>\t<bits>" per plugin
//   with at least one signal, the bits FOXAPP_PLUGIN_USE_*. Empty text clears the table. Sent on CHANGE only; the engine adds the signals it
//   knows itself (a patch node, the speakers, a text sink, a browser session, a standing duty).
#define FOXAPP_OP_PLUGIN_RUN_MODE        0x8707u /* text: plugin key; ival[0]: 0 auto, 1 always, 2 stopped */
#define FOXAPP_OP_PLUGIN_USE             0x8708u /* long text: "<file>\t<bits>" lines - what the window shows of each plugin; empty clears */
#define FOXAPP_PLUGIN_USE_WINDOW         1u /* one of the plugin's image / panel / instrument windows is shown */
#define FOXAPP_PLUGIN_USE_MAP_PAGE       2u /* the plugin's map page is open (a track source) */
#define FOXAPP_PLUGIN_USE_RADAR_SCOPE    4u /* the radar scope is showing (every track source) */
#define FOXAPP_PLUGIN_USE_DECODER_OUTPUT 8u /* the Decoder output window is open (a text decoder) */
#define FOXAPP_PLUGIN_USE_ALL            15u
// THE STORE'S SITES, PICTURES AND CLEAN-UP (stub campaign B3, 0.99.49 - 0.99.74; docs/api-0.3-additions.md 9.4), all LOCAL sessions only and
// none of them on the web profile: a REMOTE session can neither add a source of code nor start a transfer to an address a stranger chose.
//   STORE_SITE (ADMIN): text: the site's address as typed (it is normalised and re-checked here: PluginRepo::normaliseSiteUrl - https only, no
//   credentials, a host - and refused when it is already listed, is the official address, or the list is full); ival[0]: 0 ADD, 1 REMOVE;
//   ival[1] (ADD): 1 = the user ticked the box that says what a site is (the consent) - without it ADD is refused and nothing is written.
//   ADD starts a catalogue read. Neither runs under a transfer in flight (BUSY). The refusal's sentence is the result's message and
//   fact.store.siteError. REMOVE leaves what was installed from the site installed.
//   STORE_PICTURES_WANT (ADMIN): text: a catalogue KEY (the id, or "<id>@<site label>"); ival[0]: 1 = ask again for a picture that failed.
//   Queues the entry's pictures (one transfer at a time, after any read or download); NOT_FOUND for a key the catalogue does not hold.
//   STORE_CLEAN_OLD (ADMIN): ival[0] 0: remove EVERY old copy the last scan found superseded (the store's CLEAN UP key, once confirmed - the
//   window asks first; fact.store.oldCopies is what it lists); ival[0] 1: REMOVE FILE on the orphaned module named in text, which is judged
//   again against the engine's own scan. Not under a transfer (BUSY).
#define FOXAPP_OP_STORE_SITE             0x8709u /* text: the site's address; ival[0]: 0 add, 1 remove; ival[1] (add): 1 = consent given */
#define FOXAPP_OP_STORE_PICTURES_WANT    0x870Au /* text: catalogue key; ival[0]: 1 = retry a failed picture */
#define FOXAPP_OP_STORE_CLEAN_OLD        0x870Bu /* ival[0]: 0 remove every old copy, 1 remove the orphaned file named in text */
// A PLUGIN'S SETTINGS FORM, edited (stub campaign B9, 0.99.43; docs/api-0.3-additions.md 9.7), LOCAL sessions only and not on the web profile:
//   PLUGIN_SETTING_EDIT (SETTINGS): text (or long text): "<module file>\x1F<key>\x1F<value>" - the module's file name (PLUGINS' detail), the field's key
//   (PLUGIN_SETTINGS' spec fact) and the text the person typed or chose, exactly as typed (the value may be empty: a cleared box is a value, not a
//   deletion). The ENGINE judges it (core::commitSettingEdit: the field's own length and kind, UTF-8, no control character, a number that is one) and
//   writes it through PluginApiCore::settingsUiSet - the one path that advances the plugin's settings_seq, once for an accepted edit that changes the
//   value and never for a refused one. The ANSWER carries the verdict: OK for an accepted edit, otherwise a refusal whose applied[0] is the
//   core::SettingEditResult (1 TooLong, 2 NotANumber, 3 BadText, 4 StoreFull, 5 StoreError, 6 Refused) and whose message says it in English; NOT_FOUND
//   for a module that is not loaded or declares no such field. applied[0] is 0 for an accepted edit.
#define FOXAPP_OP_PLUGIN_SETTING_EDIT    0x870Cu /* text: "<module file>\x1F<key>\x1F<value>"; answered with the verdict in applied[0] */
// THE TRANSMIT GRANT (stub campaign B13; see FOXAPP_OP_TX_PLUGIN_ARM, 0x842B, for the whole of the safety argument). The third of a plugin's grants, and the ONLY
// one that is NOT a kind of FOXAPI_OP_PLUGIN_GRANT: that op is on the web profile, so the grant that lets a plugin key a radio has an op of its own, LOCAL sessions only,
// grant TRANSMIT, never on the web profile and refused to every REMOTE session. It is worth nothing alone: the plugin keys only while the operator's arming window is open too.
//   PLUGIN_TX_GRANT (TRANSMIT): text: the module's file name; ival[0]: 1 grant, 0 revoke. A grant is for a LOADED module that declares CASCADE_CAP_TX_AUDIO
//   (NOT_FOUND otherwise); a revoke is accepted for any name (a grant held for a module that has gone can still be taken back). The grant is saved
//   (AppConfig::pluginTransmitAllowed); the arming window is not, ever. A revoke unkeys, and closes the window when it was the last grant, in the same pass.
#define FOXAPP_OP_PLUGIN_TX_GRANT        0x870Du /* text: the module file; ival[0]: 1 grant / 0 revoke */
// fact.plugin.<module file>.tx: ONE DECIMAL NUMBER, the bits of what the plate says about a module that declares CASCADE_CAP_TX_AUDIO (core::PluginRunner::txBits plus the two the
// engine adds). The window turns them into the plate's sentence (gui::txPlateState); the engine never words it.
#define FOXAPP_TXBIT_DECLARED  1u   /* the runner has a transmit-audio instance of it */
#define FOXAPP_TXBIT_ASKED     2u   /* its active() has been non-zero at least once this session: the GRANT TRANSMIT key is offered */
#define FOXAPP_TXBIT_HOLDER    4u   /* it holds the transmitter's audio now */
#define FOXAPP_TXBIT_WAITING   8u   /* granted and wanting the key, but another plugin holds it */
#define FOXAPP_TXBIT_STOPPED   16u  /* its pull() reported a permanent error: stopped for good */
#define FOXAPP_TXBIT_GRANTED   32u  /* it holds the TRANSMIT grant */
#define FOXAPP_TXBIT_ON_AIR    64u  /* the transmitter is keyed and this is the plugin it is keyed for */
// The Transmit page's open flag (engine/stage3b-pre 2b, docs/engine-stage3.md
// OPEN 1/10, SAFETY): ival[0] 0/1. Since engine stage 4 the flag gates NO key:
// the consent the window withdraws when its page closes (gui::txKeyCommands) is
// what releases a remote key held. This op only moves the flag, at once, for
// the normal UI path (the Transmit toolbar switch, the page's own close button).
#define FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN 0x8408u /* ival[0]: 0/1 - closing releases a remote key held, at once */
// A typed frequency the Source panel could not parse (engine/stage3b-pre 2c,
// docs/engine-stage3.md OPEN 1): text, or empty to CLEAR the line - the
// window's own validation, never a radio's answer (every radio-side error
// already reaches sourceError_ from inside the Engine).
#define FOXAPP_OP_SET_SOURCE_ERROR       0x840Au /* text: the red line under the Source controls; empty clears it */
// The Bookmarks panel's own note (an export's result - an import's note is
// set by the Engine itself, from FOXAPI_OP_BOOKMARK_IMPORT).
#define FOXAPP_OP_SET_BOOKMARK_NOTE      0x840Bu /* text: replaces bookmarkImportNote_ */
// A GROUP OF ONE-WAY "FORGET THIS" LINES (engine/stage3b-pre 2c): each
// resets exactly one status line/flag the window used to clear or consume in
// place once its purpose was served, back to its default - never anything a
// radio, the GPS reader, a scan or a decoder might still need to report a
// NEW instance of a moment later (this op only ever narrows what is shown,
// never widens it, which is why one enum-selected op can cover all of them
// safely). See cascade::core::cmd::FoxAppStatus below for the ival[0] values.
#define FOXAPP_OP_CLEAR_STATUS           0x840Cu /* ival[0]: FoxAppStatus - which line/flag to reset to its default */
// Fields-to-commands round 2 (engine/stage3b-pre, docs/engine-stage3.md OPEN
// item 1): a window-side DRAFT buffer, committed by a command when the
// operator is done editing - never the engine field itself, keystroke by
// keystroke.
#define FOXAPP_OP_SET_CATALOGUE_URL      0x840Du /* text: pluginCatalogueUrl_, committed on deactivate-after-edit */
// plutoUri_ and transmitArgs_: the window edits a LOCAL draft buffer (never
// the engine field) and, on "Open", sends the draft's text as
// FOXAPI_OP_SELECT_SOURCE "open-pluto:uri=..." / FOXAPI_OP_TX_OPEN, whose
// handlers persist the field from that same text. Round 3 added the two ops
// below so each edit is ALSO committed as it is typed: before the drafts, an
// address typed and never opened was still what config.json kept, and it is
// again.
// The scanner form needs NO new op at all: scanStartMhz_/scanStopMhz_/
// scanStepKhz_ commit through the EXISTING FOXAPP_OP_SCANNER_RANGE (bits
// 1/2/4, num[0..2] in Hz - the same op the web remote already uses; bit 8
// folds in the panel's own reconfigure-if-running check), and
// scanDwellMs_/scanHoldMs_/scanResumeMs_/scanListenMs_ commit through the
// EXISTING FOXAPI_OP_SCANNER_CONFIG (num[0..3], which already
// reconfigures-if-running on its own - found only after a from-scratch
// FOXAPP_OP_SCANNER_TIMING briefly duplicated it byte for byte).
#define FOXAPP_OP_SOUND_CARD_FORM        0x8412u /* text: device '\x1F' hostApi; ival[0]: 1 IqStereo | 2 channel right | 4 swapIq | 8 pickedFromList; num[0]: cardRateHz; num[1]: iqCentreHz; ival[1]: the FIELD MASK, FOXAPP_SOUND_CARD_FIELD_* (0 = the whole form) (gui::soundCardFormCommand / soundCardFormFromCommand, engine/soundcard_panel.hpp) */
#define FOXAPP_OP_PATCH_LOOK_FOR_RADIOS  0x8413u /* no args: "Look for radios" pressed in the patch radio inspector */
// "Stop and resume sound" needs no new op: FOXAPP_OP_DECODER_STOP_LIST
// already covers it, unchanged - the mute lifts on its own once the engine's
// own mute state machine (advanceMutePopup) sees the decoder actually stop.
#define FOXAPP_OP_MUTE_KEEP_RUNNING      0x8414u /* no args: "Keep it running" pressed - sets muteKeptRunning_ and closes the subject together, one decision */
// The Airspy R2/Mini panel (0.99.41, engine/stage3b-pre Airspy round, OPEN
// 2/3): drawAirspyControls used to call Engine::chooseAirspyDecimation/
// GainMode/Agc directly (kControlMayCall) and read the open device through
// Engine::asAirspyDevice()'s raw pointer, every frame. Now: commands, queued
// like every other gain control in the Source section (the figure is read
// back from the published state on the next frame the readback lands on -
// see drawSourceSection's own gain sliders), and the panel reads its display
// state from PublishedState::app's airspy* fields (receiver_snapshot.hpp)
// instead of the pointer.
#define FOXAPP_OP_AIRSPY_DECIMATION      0x8415u /* ival[0]: the decimation factor (1/2/4/8/16/32/64) */
#define FOXAPP_OP_AIRSPY_GAIN_MODE       0x8416u /* ival[0]: cascade::source::AirspySource::GainMode */
#define FOXAPP_OP_AIRSPY_AGC             0x8417u /* ival[0]: 0 LNA, 1 Mixer; ival[1]: 0/1 on */
// Engine round 3 fix (docs/review-harness/engine-round3-review.md, 4 and 7).
#define FOXAPP_OP_SET_PLUTO_URI          0x8418u /* text: the Pluto address box, committed as typed (plutoUri_) */
#define FOXAPP_OP_SET_TRANSMIT_ARGS      0x8419u /* text: the Transmit address box, committed as typed (transmitArgs_); refused while a board is open */
// THE DESKTOP'S OWN USAGE-REPORTING SWITCH. FOXAPI_OP_TELEMETRY_ENABLE is the
// API's slot for the same thing, and applyCommand REFUSES it (FOXAPI_DENIED)
// whoever sends it: reporting is consent given at this machine, and a plugin,
// a browser or a future API session must never be able to switch it on
// silently. An extension op is never sent across a transport (see the top of
// this file), so this one is reachable only from the desktop's checkbox.
#define FOXAPP_OP_TELEMETRY_CONSENT      0x841Au /* ival[0]: 0/1. On mints an install id only if reporting was off; off forgets it */
// THE PATCH GRAPH AS ONE COMMAND (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 6, Design A). The Patch page edits a draft and sends the whole of it;
// the Engine replaces patchGraph_ with it, or refuses it and keeps the graph
// it had. The text is core::patch::graphCommandText's (core/patch_draft.hpp):
// the config's own patch document, ids kept, plus next-id and centre-chosen
// lines the document parser already skips. Always long text in practice.
#define FOXAPP_OP_PATCH_SET_GRAPH        0x841Bu /* text (long text): the graph; ival[0]: FOXAPP_PATCH_GRAPH_* flags */
// 0x841C was FOXAPP_OP_WEB_CONTROL_STOPPED (engine/stage3b-pre, docs/engine-stage3.md OPEN 7 (c)/10): "the server that carries a browser's
// transmit key has been stopped, disabled or never started", sent by the window and, since the pump move, by the pass's standing condition.
// RETIRED in stage 5d (docs/engine-stage4.md 6.12): the web server's engine session closes when the server stops, and a closed session's key
// is opened by the key rules at the next pass. The number is not reused.
// THE SOUND CARD LIST IS WANTED (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 2): the Source section's sound card row is showing. The Engine lists
// the cards on its worker unless they are listed already, a list is already
// being taken, or a card is opening - the test the panel used to make itself
// before calling Engine::scanSoundCards directly. Sent every frame the row is
// shown unlisted; idempotent.
#define FOXAPP_OP_SOUND_CARDS_WANTED     0x841Du /* no args */
// THE STORE WINDOW'S ADD ALL KEY (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 5): the ENGINE plans the run from its own catalogue and inventory; the
// command carries only what the engine cannot know - ival[0], the ADD ALL
// tick (every maker's notice acknowledged), and text, the catalogue id of
// the ONE module whose own notice tick is on (the store's selected row), or
// empty. FOXAPI_OP_STORE_UPDATE_ALL is the same with no per-module tick.
#define FOXAPP_OP_STORE_ADD_ALL          0x841Eu /* ival[0]: 0/1 every notice acknowledged; text: the one module id acknowledged by its own tick */
// A PANEL WAS OPENED (engine stage 4, window port group 3): the anonymous
// usage report lists which of the window's pages were used this run, and the
// window used to hand each page's name to the engine from its own frame, every
// frame the page was open. The page now SAYS it once, as this op: text, the
// panel's name. The Engine records it once (a name already listed is not
// listed twice) and stops at 16 names, exactly as the direct call did. An
// empty name is BAD_ARGUMENT. LOCAL sessions only (every app op is): a remote
// session's pages are not this installation's usage. Grant VIEW: it reads and
// changes nothing but that list. Not one of the "safer" commands, so never
// merged - a repeat is answered by the engine's own de-dupe, and the window
// sends each name once a run (AppWindow::uiNotePanel).
#define FOXAPP_OP_NOTE_PANEL             0x841Fu /* text: the panel's name, as the usage report lists it ("radar", "map") */
// A SOUND CARD CHOSEN FROM THE LIST (engine stage 4, window port group 11, API 0.3 8.10): the Source section's
// Device combo sends the entry it was clicked on, and the ENGINE does what the combo's own handler did - the card's
// name and host API into the form (marked as chosen from the list), the "not connected" line cleared, and the rate
// kept if the new card offers it in the form's format, else its default, else its highest. ival[0]: the
// SOUND_CARDS entry's id (index + 1); NOT_FOUND for none, and before the cards are listed. Grant SETTINGS, LOCAL
// sessions only (every app op is).
#define FOXAPP_OP_SOUND_CARD_PICK        0x8420u /* ival[0]: the SOUND_CARDS entry's id */
#define FOXAPP_OP_SET_PATCH_PAGE_OPEN    0x8421u /* ival[0]: 0/1 - the standard window's patch view is showing (API 0.3 8.12); the engine's pass runs the patch while it is, and stops it when it goes (default: showing) */
// THE PATCH RADIO'S RECORDINGS LISTED (API 0.3 8.12): the engine reads the I/Q recordings of the record folder and of
// FOXSDR_PATCH_SAMPLES (at most kMaxRecordingOpens headers opened, a file already read is not opened again), on its own
// pass, and PATCH_DEVICES (the recordings' rows, with their rates) and RECORDINGS kind 3 change as the answer. The Radio
// part's device list asks when it opens and "Look for radios" asks again. No arguments. Grant SETTINGS, LOCAL sessions only
// (every app op is).
#define FOXAPP_OP_PATCH_LIST_RECORDINGS  0x8422u /* no arguments - list the I/Q recordings a patch Radio can play (API 0.3 8.12) */
// THE TRACK-INFO PLUGIN'S STATUS LINE (the pump move, step P2): the window owns the plugin's cache (GUI thread only) and polls its
// text in its own frame; each line it finds is sent here and lands in the Decoder output backlog as "Aircraft info", counted like
// every other line. Replaces the host hook the engine's pass used to ask (EngineHost::drainTrackInfoText). Grant VIEW (it reads and
// changes nothing but that backlog), LOCAL sessions only (every app op is).
#define FOXAPP_OP_TRACK_INFO_LINE        0x8423u /* text (or long text): one status line from the track-info plugin; empty is BAD_ARGUMENT */
// THE 0.99.84 MERGE'S OPS (docs/engine-merge-0.99.84.md; docs/api-0.3-additions.md section 9): what master's window did with engine fields in place and
// the table had no op for. Same rules as every app op: LOCAL sessions only, grant CONTROL unless said.
#define FOXAPP_OP_SET_RTLTCP_ADDR        0x8424u /* text: the rtl_tcp address box, committed as typed (rtlTcpAddr_); the Pluto box's twin */
#define FOXAPP_OP_BANDWIDTH_DRAG_END     0x8428u /* the drag on the spectrum's band edge has settled (the mouse is let go): one log line for the width it ended on */
#define FOXAPP_OP_TEST_HOLD_AUDIO_LOCK   0x8427u /* ival[0]: ms (0.99.68, bounded runs only: FOXSDR_HOLD_AUDIO_LOCK) - the first stop a control request makes of a running receiver first has a thread take the audio mutex for that long; 0 = no seam */
#define FOXAPP_OP_SET_PPM                0x8426u /* ival[0]: the crystal correction's switch (0/1); num[0]: the open radio's value, ppm (core::sanitisePpm; 0 = none) */
#define FOXAPP_OP_SET_AUDIO_BUFFER       0x8425u /* ival[0]: the Sinks rail's AUDIO BUFFER, 0 AUTOMATIC | 120 | 240 | 480 | 960 ms (sink::validAudioBufferSetting) */
// THE AIRBAND MONITOR (stub campaign B6, 0.99.60 - 0.99.71; docs/api-0.3-additions.md 9.6), LOCAL sessions only, grant TUNE (it tunes the receiver):
// several ticked AM and NFM channels off the receiver's one radio, mixed to the speakers, each behind its own squelch. ival[0] is the action:
//   0 STOP: the monitor stops (NO_CHANGE when it was neither listening nor waiting).
//   1 LISTEN: text: the preset to play - a bookmark group - or "" for every ticked AM or NFM row. Nothing ticked there is NO_CHANGE with the note
//   "Tick at least one AM or NFM frequency to listen to."; a radio that cannot be divided to an audio rate, or a block the radio will not tune to, is
//   FAILED with the note. A patch view that is showing is handed back FIRST: the engine closes it (the receiver view) and the monitor starts on the
//   pass after the patch has let go of the radio - applied[0] is then 2. A monitor already listening takes the new preset and starts again.
//   2 NEXT BLOCK: moves on to the next block now (NO_CHANGE with one block, or not listening).
//   3 ABOVE THE NOISE: the squelch to 8 dB over the median channel level the strips have reported (NO_CHANGE with no level yet); applied[1]: the dB.
//   4 SQUELCH: num[0] dB, clamped to -110..0 (applied[1]); a listening monitor rebuilds its block with it. 5 HOLD: num[0] seconds, clamped to 0..60: how
//   long a block stays after its last word; a listening multi-block monitor starts again.
//   6 TUNE ONE: ival[1]: a bookmark's id. The monitor stops ("Tuned to <name> alone.") and the receiver tunes to that row, as a click on it would.
// applied[0] is the state after: 0 not listening, 1 listening, 2 waiting for the patch to hand the radio back. What it is doing is the facts
// fact.airband.* and the live list AIRBAND_CHANNELS.
#define FOXAPP_OP_AIRBAND                0x8429u /* ival[0]: 0 stop / 1 listen (text: preset) / 2 next block / 3 above the noise / 4 squelch / 5 hold / 6 tune one */
// AUTOMATIC SQUELCH (stub campaign B12, 0.99.74; docs/api-0.3-additions.md 9.9). The AUTO key under the top bar's SQUELCH dial: while it is on, the ENGINE keeps the
// squelch threshold 8 dB above the noise floor it measures under the channel (the quietest of the last ten seconds of the meter, a carrier that holds the gate open
// never teaching it - core/noise_floor_tracker.hpp) or, while the Airband monitor listens, applies the monitor's "above the noise" rule to the monitor's own squelch
// every quarter second. LOCAL sessions only and not on the web profile; grant SETTINGS.
//   ival[0]: 0 off / 1 on. OK for what it already is (nothing changes). Turning it on forgets any floor measured before; applied[0] is the switch after.
// A HAND ON ANY THRESHOLD IS A MANUAL CHOICE: an accepted SET_SQUELCH (the dial, the Radio section's slider, the keys, a web page), an accepted AIRBAND squelch (4) and
// an AIRBAND above-the-noise (3) pressed while the monitor listens (whether or not the rule then finds a level) turn it OFF, whoever sent them; a refused or out-of-range
// one, and an above-the-noise with nothing listening, change nothing. A threshold restored from the config does not. The switch is saved (AppConfig::squelchAuto) and the
// switch and the floor are published as fact.squelch.auto / fact.squelch.floorDb.
#define FOXAPP_OP_SET_SQUELCH_AUTO       0x842Au /* ival[0]: 0 off / 1 on; applied[0]: the switch after */
// A PLUGIN MAY KEY THE TRANSMITTER (stub campaign B13, 0.99.74's CASCADE_CAP_TX_AUDIO; docs/api-0.3-additions.md 9.10). SAFETY-CRITICAL, and these three ops are the whole of
// the table's surface for it. EVERY ONE IS LOCAL-ONLY: grant TRANSMIT, not in the web profile (docs/api-0.3-additions.md 8.16.3's 38 ops do not grow), refused to every REMOTE
// session at submit (three times over: the application's ops, TRANSMIT being a web remote's PTT and nothing else, the profile) and again where it is applied. NONE OF THEM RIDES
// ON FOXAPI_OP_PLUGIN_GRANT, which IS in the web profile: its kinds stay 1 (tune) and 2 (settings), and kind 3 is still "no such grant".
//   TX_PLUGIN_ARM: the operator's ARMING WINDOW, which the ENGINE holds (a window crash, a frozen window or a closed page cannot leave a plugin able to key). It belongs to the SESSION
//     that opened it (its LEASE) and lasts while that session is alive and keeps saying the TRANSMIT page is on screen.
//       ival[0]: 5 / 15 / 60  open (or restart) the window for that many minutes; refused with a reason (applied[0] 1 no radio is open, 2 no plugin holds the grant) otherwise
//                0            close it (ival[1]: 0 "you pressed OFF", 1 "the TRANSMIT page was closed")
//                -1           THE PAGE IS LIVE, said again (the window sends it every frame the page is drawn while a window is open); anything else is OUT_OF_RANGE.
//     applyCommand REFUSES it (a window is a session's, as a PTT is): only the table's host, which knows the sender, applies it.
#define FOXAPP_OP_TX_PLUGIN_ARM          0x842Bu /* ival[0]: 5 / 15 / 60 arm, 0 close, -1 the page is live; ival[1]: 0 OFF pressed / 1 page closed */
//   TX_DATA_FLAT: the page's DATA (flat) tick, for a plugin's audio only: on (the default) it goes out as NFM with the pre-emphasis off, whatever mode the page shows. Session-only
//     (never saved). ival[0]: 0 / 1.
#define FOXAPP_OP_TX_DATA_FLAT           0x842Cu /* ival[0]: 0 off / 1 on */
// FOXAPP_OP_SOUND_CARD_FORM's ival[1], the FIELD MASK (API 0.3 8.10): which fields the command carries. 0 (and
// ALL) is the whole form. With a mask, the fields not named keep the engine's own values.
#define FOXAPP_SOUND_CARD_FIELD_DEVICE   1  /* text: device and host API */
#define FOXAPP_SOUND_CARD_FIELD_FORMAT   2  /* ival[0] bit 1 */
#define FOXAPP_SOUND_CARD_FIELD_CHANNEL  4  /* ival[0] bit 2 */
#define FOXAPP_SOUND_CARD_FIELD_SWAP_IQ  8  /* ival[0] bit 4 */
#define FOXAPP_SOUND_CARD_FIELD_PICKED   16 /* ival[0] bit 8 */
#define FOXAPP_SOUND_CARD_FIELD_RATE     32 /* num[0] */
#define FOXAPP_SOUND_CARD_FIELD_CENTRE   64 /* num[1] */
#define FOXAPP_SOUND_CARD_FIELDS_ALL     127
// FOXAPP_OP_PATCH_SET_GRAPH's ival[0]: set when the graph is a NEW DOCUMENT
// (the config's patch, a patch file) rather than an edit of the graph the
// engine already has. The Engine counts these (Engine::patchGraphEpoch_), so a
// window part-way through an edit of the old document knows not to put that
// edit on top of the new one.
#define FOXAPP_PATCH_GRAPH_DOCUMENT      1
// FOXAPP_OP_PATCH_SET_GRAPH's ival[1] (API 0.3 8.12): the graph VERSION the
// sender's draft was last in step with (fact.patch.version, read in the same
// publish as the graph it edited). 0 is "unconditional" (a document load, and
// today's behaviour). A version that is not the current one is MERGED by the
// engine (core::patch::rebaseDraft against the retained graph of that
// version) and not overwritten; one the engine no longer retains is refused
// FOXAPI_FAILED. The result's applied[0] is the version after the command and
// applied[1] is 1 when the engine merged.

#define FOXAPP_OP_FIRST 0x8000u
#define FOXAPP_OP_LAST  0x8FFFu

namespace cascade::core::cmd {

// FOXAPP_OP_CLEAR_STATUS's ival[0]. Each resets exactly one field a status
// panel used to clear or consume in place; see the field named in each
// comment (engine.hpp) for what it means and who else writes it.
enum FoxAppStatus : std::int64_t {
    FOXAPP_STATUS_GPS_REFUSAL = 1,               // gpsRefusal_.clear()
    FOXAPP_STATUS_SOUND_CARD_MISSING = 2,         // soundCardMissing_.clear()
    FOXAPP_STATUS_DECODER_LOG = 3,                // decoderLog_.clear()
    FOXAPP_STATUS_SOAPY_SCAN_DEFERRED_LOGGED = 4, // soapyScanDeferredLogged_ = false
    FOXAPP_STATUS_MUTE_POPUP_QUEUED = 5,          // mutePopupQueued_ = false (consumed)
    FOXAPP_STATUS_GPS_RESULT = 6,                 // gpsReader_.clearResult(): a finished read's line goes (a no-op while it listens)
};

// A command and, when its text did not fit FoxCommand::text, the whole of it.
struct QueuedCommand {
    FoxCommand c{};
    std::string longText;
};

// A zeroed command of `op`, structSize set - every slot an op does not read
// stays zero, as the API requires.
inline FoxCommand make(std::uint32_t op) {
    FoxCommand c;
    std::memset(&c, 0, sizeof(c));
    c.structSize = static_cast<std::uint32_t>(sizeof(FoxCommand));
    c.op = op;
    return c;
}

inline FoxCommand makeInt(std::uint32_t op, std::int64_t v0, std::int64_t v1 = 0) {
    FoxCommand c = make(op);
    c.ival[0] = v0;
    c.ival[1] = v1;
    return c;
}

inline FoxCommand makeNum(std::uint32_t op, double n0, double n1 = 0.0) {
    FoxCommand c = make(op);
    c.num[0] = n0;
    c.num[1] = n1;
    return c;
}

// num[0] and ival[0] together (STEP_TUNE's steps and size, a click-to-tune's
// frequency and snap, a split's frequency and switch).
inline FoxCommand makeNumInt(std::uint32_t op, double n0, std::int64_t v0) {
    FoxCommand c = make(op);
    c.num[0] = n0;
    c.ival[0] = v0;
    return c;
}

// Puts `s` into c.text (truncated, always NUL-terminated) and answers the
// long text to carry beside it: empty when it fitted, the whole of `s` when
// it did not.
inline std::string putText(FoxCommand& c, const std::string& s) {
    const std::size_t n = std::min(s.size(), static_cast<std::size_t>(FOXAPI_TEXT_CHARS - 1));
    std::memcpy(c.text, s.data(), n);
    c.text[n] = '\0';
    return (s.size() > n) ? s : std::string();
}

inline QueuedCommand makeText(std::uint32_t op, const std::string& s, std::int64_t v0 = 0,
                              std::int64_t v1 = 0, double n0 = 0.0) {
    QueuedCommand q;
    q.c = make(op);
    q.c.ival[0] = v0;
    q.c.ival[1] = v1;
    q.c.num[0] = n0;
    q.longText = putText(q.c, s);
    return q;
}

// The text an op reads: the long text when one came with it, else the slot
// (read only up to its NUL, never past the array).
inline std::string textOf(const FoxCommand& c, const std::string& longText) {
    if (!longText.empty()) { return longText; }
    const void* nul = std::memchr(c.text, '\0', sizeof(c.text));
    const std::size_t n = (nul != nullptr) ? static_cast<std::size_t>(static_cast<const char*>(nul) - c.text)
                                           : sizeof(c.text);
    return std::string(c.text, n);
}

// Mode names in FOXAPI_DEMOD_* order: NFM is 1 ... RAW is 8, the same order
// as the desktop's mode keys (and CASCADE_DEMOD_*). 0 for a name it does not
// carry, which applyCommand refuses as out of range.
inline std::uint32_t foxDemodFromName(const char* name) {
    static constexpr const char* kNames[8] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
    if (name == nullptr) { return 0u; }
    for (std::uint32_t i = 0; i < 8u; ++i) {
        if (std::strcmp(kNames[i], name) == 0) { return i + 1u; }
    }
    return 0u;
}

// The op's name for logs, docs and tests ("SET_CENTRE", "APP_VFO_TO_ABSOLUTE");
// "?" for an op nothing here knows.
const char* opName(std::uint32_t op);

// True for every op applyCommand implements - the API's and the extensions.
bool isKnownOp(std::uint32_t op);

inline bool isAppOp(std::uint32_t op) { return op >= FOXAPP_OP_FIRST && op <= FOXAPP_OP_LAST; }

// Every op isKnownOp answers true for, in a fixed order (tests, docs).
const std::uint32_t* knownOps(std::size_t& count);

}  // namespace cascade::core::cmd
