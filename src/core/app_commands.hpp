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

#define FOXAPP_OP_FIRST 0x8000u
#define FOXAPP_OP_LAST  0x8FFFu

namespace cascade::core::cmd {

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
