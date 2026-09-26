// receiver_tables.hpp - the receiver's fixed tables, constants and small
// rules, shared by the Engine (which runs the receiver) and the window (which
// draws it).
//
// Moved VERBATIM out of gui/app_window.cpp's, app_window_patch_radios.cpp's
// and app_window_soundcard.cpp's file-local namespaces and gui/app_window.hpp
// (engine extraction stage 3a): once the machinery that reads them lives in
// src/engine, one copy has to be reachable from both sides of the line, and a
// table written twice is two tables that will one day disagree. The only
// change: a function defined here is `inline`, as a function defined in a
// header has to be. Kept in namespace cascade::gui, like the other helpers
// that moved, so every caller reads exactly as before.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_RECEIVER_TABLES_HPP
#define CASCADE_ENGINE_RECEIVER_TABLES_HPP

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/receiver_position.hpp"
#include "dsp/demod.hpp"
#include "net/status_compose.hpp"

namespace pc = cascade::core::patch;

namespace cascade::gui {

// Pipeline configuration. 2 MS/s at FFT 1024 publishes far more frames than
// the GUI's ~60 fps polls; the latest-frame slot in Pipeline absorbs the
// difference by design. Alpha 0.5 smooths the trace without visible lag.
constexpr double kSampleRateHz = 2'000'000.0;
constexpr std::size_t kFftSize = 1024;
constexpr float kAveragingAlpha = 0.5f;

// The one door a receiver position comes in by (core/receiver_position.hpp),
// under the name the moved code calls it by - gui/scope_view.hpp brings it
// into this namespace the same way.
using cascade::core::receiverPositionAcceptable;

// Whether a device open that finished on a worker thread should still be
// applied to the pipeline.
//
// Opening a SoapySDR device takes seconds, and the user is not frozen while it
// happens: they can pick the built-in generator, open an IQ file, or drive
// either from the web UI in the meantime. Without this check the open simply
// landed when it landed and replaced whatever they had chosen — the radio
// changed itself, several seconds after they told it not to.
//
// The rule is a sequence number rather than a "cancelled" flag because the
// question is not "was it cancelled" but "is the answer still about the source
// the user is looking at": every install of a source bumps the counter, the
// request records the value it was made at, and a result whose value has moved
// on is about a source selection that no longer exists.
inline bool asyncOpenStillWanted(std::uint64_t requestedAtGen,
                                 std::uint64_t currentGen) {
    return requestedAtGen == currentGen;
}

// --- the bandwidth the combo offers, and the bandwidth it is actually running
//
// OUT HERE SO SOMETHING CAN CHECK THEM. These two were file-private in
// app_window.cpp, which is why the fault they now close shipped: nothing in
// the suite can construct an AppWindow, so anything private to that file is
// reachable only by running the application and looking at it. The pair below
// is pure arithmetic over a table, so it costs nothing to put where a test can
// see it - and this is the same reason railRowHeight and railChipReserve are
// here rather than there.
//
// The steps the Bandwidth combo offers, widest first. app_window.cpp letters
// them from kBwLabels, which must stay in step with this.
inline constexpr double kBwHz[6] = {200000.0, 150000.0, 12500.0, 10000.0, 6000.0, 3000.0};
inline constexpr int kBwCount = static_cast<int>(sizeof(kBwHz) / sizeof(kBwHz[0]));

// WHICH OF THE OFFERED STEPS A BANDWIDTH IS, OR -1 FOR NONE OF THEM.
//
// A "nearest step" answer was the fault. A plugin preset may ask for a
// bandwidth this table does not carry - the NOAA APT preset asks for 40 kHz,
// because an APT signal is about 34 kHz wide and 12.5 kHz slices its video
// sidebands off - and the nearest step to 40 kHz is 12.5 kHz. Pointing the
// combo there made it letter a bandwidth the receiver was not running, and
// because every control that writes this index back writes kBwHz[index] with
// it, the next touch of any of them really did narrow the receiver to the
// figure the combo had been wrongly showing.
//
// An eighth of a hertz of tolerance because both sides are exact table values,
// or a config round-trip of one, never a computed quantity.
inline int bandwidthStepIndex(double hz) {
    for (int i = 0; i < kBwCount; ++i) {
        const double d = kBwHz[i] - hz;
        if ((d < 0.0 ? -d : d) < 0.125) { return i; }
    }
    return -1;
}

// The bandwidth as the combo letters it, in kBwLabels' own shorthand, for a
// value that may be none of them: "200k", "40k", "12.5k", "3k". Three decimals
// of a kilohertz is one hertz, and the trailing zeros come off, so a table
// value reads exactly as its own label does and a preset's own figure reads as
// itself rather than as the nearest word in the table. Nothing below 3 kHz can
// arrive - kVfoBwMinHz is the floor every writer clamps to.
inline void formatBandwidth(double hz, char* out, std::size_t n) {
    int len = std::snprintf(out, n, "%.3f", hz / 1000.0);
    if (len < 0 || static_cast<std::size_t>(len) >= n) { return; }
    while (len > 0 && out[len - 1] == '0') { out[--len] = '\0'; }
    if (len > 0 && out[len - 1] == '.') { out[--len] = '\0'; }
    std::snprintf(out + len, n - static_cast<std::size_t>(len), "k");
}

// --- from gui/app_window.cpp ----------------------------------------------

// THE PLUTO'S DRIVER KEY, spelled once. The Source section has to recognise
// its row in three places (the label, the row that must not open on
// selection, and the address field it shows instead), and a literal in each
// is three chances for a typo that compiles.
constexpr const char* kPlutoDriverKey = "pluto";

constexpr const char* kModeNames[8] = {"NFM", "WFM", "AM", "DSB",
                                       "USB", "CW",  "LSB", "RAW"};

// The web and CAT readers name the mode from the published FOXAPI_DEMOD_*
// (net::kDemodNames, indexed modeIndex_ + 1 - gui::abiDemodForModeIndex):
// the two tables must be one list, word for word.
static_assert(
    [] {
        for (int i = 0; i < 8; ++i) {
            if (std::string_view(kModeNames[i]) != std::string_view(cascade::net::kDemodNames[i + 1])) {
                return false;
            }
        }
        return true;
    }(),
    "kModeNames and net::kDemodNames must name the modes identically");

constexpr cascade::dsp::DemodMode kModeMap[8] = {
    cascade::dsp::DemodMode::NFM, cascade::dsp::DemodMode::WFM,
    cascade::dsp::DemodMode::AM,  cascade::dsp::DemodMode::DSB,
    cascade::dsp::DemodMode::USB, cascade::dsp::DemodMode::CW,
    cascade::dsp::DemodMode::LSB, cascade::dsp::DemodMode::RAW};

// Band-snap intervals for dragging the VFO CENTER on the spectrum, indexed
// in kModeNames order. The snap applies to the ABSOLUTE tuned frequency
// (source center + VFO offset), not the raw offset, so snapped stations land
// on real channel rasters; holding Shift bypasses it (free tuning).
//
//   mode | snap     | rationale
//   -----+----------+------------------------------------------
//   NFM  | 12.5 kHz | narrowband two-way channel raster
//   WFM  | 100 kHz  | broadcast FM channel raster
//   AM   | 9 kHz    | LW/MW broadcast raster
//   DSB  | 1 kHz    | free-form carrier work: round numbers
//   USB  | 1 kHz    | ham SSB convention
//   CW   | 1 kHz    | ham CW convention
//   LSB  | 1 kHz    | ham SSB convention
//   RAW  | 1 kHz    | diagnostics; snap kept for predictable steps
constexpr double kModeSnapHz[8] = {12500.0, 100000.0, 9000.0, 1000.0,
                                   1000.0,  1000.0,   1000.0, 1000.0};

constexpr double kDeemphUs[3] = {50.0, 75.0, 0.0};

constexpr int kDeemphCount = 3;

// VFO bandwidth clamp for edge drags and config restore:
// [3 kHz, 90% of the channel rate]. The lower bound keeps the band visible,
// grabbable and audible; the upper bound leaves the Vfo's decimating filter
// a transition band instead of demanding a brick wall at Nyquist.
constexpr double kVfoBwMinHz = 3000.0;

constexpr double kVfoBwMaxChanFrac = 0.9;

// ASCII case-insensitive equality. Used only to compare plugin FILE NAMES,
// which sanitiseFileName() has already restricted to [A-Za-z0-9._-] — so a
// byte-wise ASCII fold is the whole of the correct comparison here, with no
// locale or Unicode case-folding question to get wrong.
inline bool equalsFileNameAscii(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') { ca = static_cast<char>(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = static_cast<char>(cb - 'A' + 'a'); }
        if (ca != cb) { return false; }
    }
    return true;
}

// A NATIVE radio's MODEL WITH NO SERIAL IN IT, for the log, the crash context
// and the scan-gate caption - the rule every diagnostic line in this file
// keeps. core::sanitiseDevice does the job for a SoapySDR kwargs string (its
// allow list is driver/product/type, so a serial can never survive it), but a
// native row's args are nothing but "serial=00000001" and sanitising them
// leaves an empty string. The model has to come out of the LABEL instead,
// which is the bus-reported description with the serial appended in brackets.
inline std::string modelFromNativeLabel(const std::string& label) {
    const std::size_t at = label.find(" (serial ");
    return at == std::string::npos ? label : label.substr(0, at);
}

// "2.400 MS/s" - three decimals because the rates that differ do so in the
// third (2.048 against 2.160 against 2.400 on an RTL-SDR), and a menu that
// showed "2.0", "2.2" and "2.4" for those would be a menu with two rows a
// user cannot tell apart.
inline std::string rateLabel(double hz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f MS/s", hz / 1.0e6);
    return buf;
}

// --- from gui/app_window_patch_radios.cpp ---------------------------------

constexpr double kPatchDefaultRateHz = 2.0e6;

inline double radioRate(const pc::Node& n) { return n.rateHz > 0.0 ? n.rateHz : kPatchDefaultRateHz; }

// The Channel a Demod node is fed from, or kNoNode.
inline pc::NodeId demodChannel(const pc::Graph& g, pc::NodeId demod) {
    for (const pc::Wire& w : g.wires()) {
        if (w.to != demod) { continue; }
        const pc::Node* f = g.find(w.from);
        return (f != nullptr && f->kind == pc::NodeKind::Channel) ? f->id : pc::kNoNode;
    }
    return pc::kNoNode;
}

// --- from gui/app_window_soundcard.cpp ------------------------------------

// "192 kHz", "44.1 kHz", "7.0500 MHz": the Source section's frequency words.
inline std::string soundCardHzText(double hz) {
    char buf[40];
    if (std::fabs(hz) >= 1.0e6) {
        std::snprintf(buf, sizeof(buf), "%.4f MHz", hz / 1.0e6);
    } else if (std::fabs(std::fmod(hz, 1000.0)) < 0.5) {
        std::snprintf(buf, sizeof(buf), "%.0f kHz", hz / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f kHz", hz / 1000.0);
    }
    return buf;
}

}  // namespace cascade::gui

#endif  // CASCADE_ENGINE_RECEIVER_TABLES_HPP
