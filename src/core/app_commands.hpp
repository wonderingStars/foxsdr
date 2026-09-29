// app_commands.hpp - the ONE vocabulary every change to the receiver is said in
// (engine extraction, stage 1; docs/engine-stage1.md).
//
// WHAT THIS IS. The FoxSDR engine API (third_party/foxsdr_api/foxsdr_api.h,
// API 0.2, vendored unmodified) describes a change to the receiver as a
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
// Plugins and the store.
#define FOXAPP_OP_USER_PRESET_APPLY      0x8701u /* text: plugin key; ival[0]: ordinal among that plugin's user presets */
#define FOXAPP_OP_USER_PRESET_FORGET_AT  0x8702u /* text: plugin key; ival[0]: ordinal among that plugin's user presets */
#define FOXAPP_OP_PLUGIN_AUTO_PRESET     0x8703u /* text: plugin key; ival[0]: 0 it was started, 1 its window was opened */
#define FOXAPP_OP_DECODER_STOP_LIST      0x8704u /* text (or long text): plugin keys, one per line; ival[0]: 1 = and end the mute they caused */
#define FOXAPP_OP_STORE_UPDATE           0x8705u /* text: catalogue id of an installed plugin to update */
#define FOXAPP_OP_STORE_REMOVE_BLOCKED   0x8706u /* text: file name of a plugin the catalogue policy blocked */
// The Transmit page's open flag (engine/stage3b-pre 2b, docs/engine-stage3.md
// OPEN 1/10, SAFETY): ival[0] 0/1. Closing it (0) releases a remote transmit
// key already held, in the SAME step - the window's own per-frame poll in
// applyWebControls still does the same check as a second line of defence for
// anything that still sets transmitOpen_ directly (a test's friend accessor,
// today), but this is the immediate, zero-frame-latency release for the
// normal UI path (the Transmit toolbar switch, the page's own close button).
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
#define FOXAPP_OP_SOUND_CARD_FORM        0x8412u /* text: device '\x1F' hostApi; ival[0]: 1 IqStereo | 2 channel right | 4 swapIq | 8 pickedFromList; num[0]: cardRateHz; num[1]: iqCentreHz (gui::soundCardFormCommand / soundCardFormFromCommand, engine/soundcard_panel.hpp) */
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
// THE WEB CONTROL IS NOT RUNNING (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 7 (c)/10): the server that carries a browser's transmit key has been
// stopped, disabled or never started, so no remote key can be held - the
// Engine releases one in the same step. Sent by the window, which owns the
// server until stage 5, where it stops it and once a frame while it is not
// running (idempotent: a no-op with no remote key held).
#define FOXAPP_OP_WEB_CONTROL_STOPPED    0x841Cu /* no args */
// THE SOUND CARD LIST IS WANTED (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 2): the Source section's sound card row is showing. The Engine lists
// the cards on its worker unless they are listed already, a list is already
// being taken, or a card is opening - the test the panel used to make itself
// before calling Engine::scanSoundCards directly. Sent every frame the row is
// shown unlisted; idempotent.
#define FOXAPP_OP_SOUND_CARDS_WANTED     0x841Du /* no args */
// FOXAPP_OP_PATCH_SET_GRAPH's ival[0]: set when the graph is a NEW DOCUMENT
// (the config's patch, a patch file) rather than an edit of the graph the
// engine already has. The Engine counts these (Engine::patchGraphEpoch_), so a
// window part-way through an edit of the old document knows not to put that
// edit on top of the new one.
#define FOXAPP_PATCH_GRAPH_DOCUMENT      1

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
