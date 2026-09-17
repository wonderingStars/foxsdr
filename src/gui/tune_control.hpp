// Pure tuning decisions pulled out of AppWindow so they can be checked
// without a live source, a running pipeline or an open ImGui frame - the
// same reason gui/scope_view.hpp carries receiverPositionAcceptable rather
// than leaving it inline in a draw call.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_ui.hpp"
// normalisedSerial lives with the driver that needs it, and the prefer-native
// rule has to use the SAME normalisation the driver's own open() matches on -
// a rule stricter than the driver's would point at a device the driver then
// refuses. See nativeSerialMatches.
#include "source/airspyhf_source.hpp"
#include "source/device_source.hpp"

namespace cascade::gui {

// WHAT VFO OFFSET A PRESET SHOULD LEAVE BEHIND.
//
// An I/Q decoder (ADS-B, AIS, ...) is handed the whole raw device band and
// does its own tuning inside it, so what it needs from the receiver is the
// BAND centred on its frequency - a leftover VFO offset just walks the
// device off that centre for no reason the decoder can use. An audio
// decoder tunes through the VFO, so its offset is exactly the station the
// user chose and a preset must leave it alone.
//
// Measured 2026-09-08: a remembered -12 kHz offset made the ADS-B preset
// land the frequency counter (device centre + VFO offset - see the comment
// on AppWindow::drawFrequencyReadout) at 1089.988 MHz instead of 1090.000
// MHz, with the plugin's own window still captioned "1090.000 MHz". The
// offset is applied through the same setter path the VFO slider uses, at
// the call site, so the slider follows; this function only decides the
// number.
inline double presetVfoOffsetHz(std::uint32_t flags, double currentOffsetHz) {
    return (flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u ? 0.0 : currentOffsetHz;
}

// --- Tune mismatch: the radio answered somewhere other than it was asked ----

// More than this and the difference is the device REFUSING the request, not
// a synthesiser landing on its nearest step - measured coercion on a B200 is
// a few Hz. Named so AppWindow::applyRetuneNow and this file's test agree on
// one number instead of two copies of 500.0 drifting apart.
inline constexpr double kTuneMismatchToleranceHz = 500.0;

namespace detail {

// "70.000 MHz" below a gigahertz, "6.000 GHz" at or above it - the same
// switchover a user would make reading the number aloud, and the one that
// keeps a B200's "6.000 GHz" from printing as "6000.000 MHz".
inline std::string formatTuneFreq(double hz) {
    char buf[32];
    if (std::fabs(hz) >= 1.0e9) {
        std::snprintf(buf, sizeof(buf), "%.3f GHz", hz / 1.0e9);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f MHz", hz / 1.0e6);
    }
    return buf;
}

}  // namespace detail

// Empty when the device landed within tolerance of what was asked - the
// overwhelmingly common case, and the one this must never annotate.
//
// Outside tolerance: names both figures, adds the device's own range when
// the caller has one (hasRange - false for the signal generator and the IQ
// file, which are never asked, and for a Soapy device whose driver answered
// none), and for a PLUGIN PRESET specifically (isPluginPreset) says the
// preset itself needs a receiver that reaches that band - a preset button
// that silently retuned nowhere near its own decoder reads as the plugin
// being broken rather than the hardware being unable to follow it.
//
// Measured on a USRP B200 (range 70 MHz - 6 GHz): a request for 7.000 MHz
// answered 30.800 MHz, and a WEFAX preset for 3.853 MHz answered 34.000 MHz,
// with nothing on screen or in the log saying so.
inline std::string tuneMismatchMessage(double requestHz, double answeredHz, bool hasRange,
                                        double rangeLoHz, double rangeHiHz,
                                        bool isPluginPreset) {
    if (std::fabs(answeredHz - requestHz) <= kTuneMismatchToleranceHz) { return {}; }

    std::string msg = "This radio cannot tune to " + detail::formatTuneFreq(requestHz) +
                       " - it answered " + detail::formatTuneFreq(answeredHz) + ".";
    if (hasRange) {
        msg += " Its range is " + detail::formatTuneFreq(rangeLoHz) + " to " +
               detail::formatTuneFreq(rangeHiHz) + ".";
    }
    if (isPluginPreset) {
        msg += " This preset needs a receiver that covers that band.";
    }
    return msg;
}

// --- Auto-preset on start: "we want the user to have to do nothing" --------
//
// The owner's own words for this one. A decoder that publishes presets knows
// where it listens; a user who just pressed START on it should not then have
// to go find the preset button too, unless the receiver is ALREADY where that
// preset wants it - in which case pressing the button again would only repeat
// a tune that already happened, or worse, stomp a frequency the user dialled
// in deliberately.
//
// "Already inside" is judged per preset the same way applyPluginPreset treats
// the two kinds of decoder it can be applying: a band preset (declares
// CASCADE_PRESET_DEVICE_CENTRE) is inside when the DEVICE CENTRE sits within
// half the current sample rate of the preset's frequency - that is the whole
// band the decoder is handed, so anywhere in it counts as "already there". An
// audio preset is inside when the TUNED frequency (device centre + VFO
// offset - the same sum drawFrequencyReadout prints) is within
// max(bandwidth/2, 5 kHz) of the preset's frequency: half the channel it
// asks for, floored at 5 kHz so a preset that leaves bandwidthHz at 0 (no
// preference) still gets a sane window instead of collapsing to an exact-Hz
// match nothing would ever land on by chance.
namespace detail {

// THE CORE OF "IS THE RECEIVER ALREADY HERE", pulled out to the three plain
// numbers a preset actually contributes - frequency, bandwidth, and whether
// it is a device-centre preset - rather than a full CascadePreset, so the
// same arithmetic can be run from a cascade::core::MutePreset (the mute
// snapshot's own reduced copy of a preset, built without ever calling back
// into the plugin - see AppWindow::rebuildMuteStates) as well as from the
// ABI struct itself. presetAlreadyTunedTo below is now a thin adapter over
// this for its existing callers; nothing about its behaviour changes.
inline bool presetAlreadyTunedToFields(double presetFrequencyHz, double presetBandwidthHz,
                                        bool deviceCentre, double deviceCentreHz,
                                        double vfoOffsetHz, double deviceRateHz) {
    if (deviceCentre) {
        // Zero rate (no source open, or a source that has not answered yet)
        // collapses the tolerance to zero rather than dividing by anything -
        // "inside" then means exactly on the mark, which is the safe answer
        // when there is no band to speak of at all.
        const double toleranceHz = 0.5 * deviceRateHz;
        return std::fabs(deviceCentreHz - presetFrequencyHz) <= toleranceHz;
    }
    const double toleranceHz = std::max(0.5 * presetBandwidthHz, 5000.0);
    const double tunedHz = deviceCentreHz + vfoOffsetHz;
    return std::fabs(tunedHz - presetFrequencyHz) <= toleranceHz;
}

inline bool presetAlreadyTunedTo(const CascadePreset& ps, double deviceCentreHz,
                                  double vfoOffsetHz, double deviceRateHz) {
    return presetAlreadyTunedToFields(ps.frequencyHz, ps.bandwidthHz,
                                       (ps.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u,
                                       deviceCentreHz, vfoOffsetHz, deviceRateHz);
}

}  // namespace detail

// Which preset a plugin START should apply automatically, or -1 to leave the
// receiver alone. `presets` is whatever the plugin published, already
// filtered to the valid, in-range ones the host would ever act on (see
// AppWindow::drawPluginPresets' own filter, which this call site repeats) -
// this function does not re-validate them.
//
// -1 the moment ANY preset already matches where the receiver is, not just
// the first: a plugin with several presets that happens to be sitting in its
// third is not "somewhere else", it is already listening for this decoder,
// and retuning it to preset zero would be the one thing this feature exists
// to avoid doing.
//
// Otherwise always 0 - the FIRST preset, never a "nearest" guess - because
// that is the one button-press this replaces, and a decoder with several
// presets (WEFAX's regions, say) gets the same one a user reaching for the
// panel would have pressed first.
inline int autoPresetIndexOnStart(const std::vector<CascadePreset>& presets,
                                   double deviceCentreHz, double vfoOffsetHz,
                                   double deviceRateHz) {
    if (presets.empty()) { return -1; }
    for (const CascadePreset& ps : presets) {
        if (detail::presetAlreadyTunedTo(ps, deviceCentreHz, vfoOffsetHz, deviceRateHz)) {
            return -1;
        }
    }
    return 0;
}

// --- Auto-preset when a plugin's window is opened by hand -------------------
//
// The same owner's rule, extended to the DECODE rail: since 0.79.1 a plugin's
// window opens ONLY when a row is pressed (drawPluginWindowRows,
// drawMapPageSections) - never by itself, and never restored from a saved
// "open" flag (PluginWindows starts empty at every launch; MapPage::open is
// cleared by core::startupState). Pressing that row is therefore exactly as
// deliberate an "I want this plugin now" as pressing START, and the owner's
// "the user has to do nothing" applies the same way.
//
// This decides only WHETHER a row's click just performed that gesture - not
// what to do about it, which is AppWindow::maybeAutoPresetOnShow sharing
// maybeAutoPresetOnStart's own decision (autoPresetIndexOnStart above) and
// apply path. A row is a TOGGLE, so the same click that shows a window also
// hides one, and only the shown case is "give me this plugin":
//
//   clicked && !wasShownBeforeClick   -> true   (hidden -> shown: the gesture)
//   clicked &&  wasShownBeforeClick   -> false  (shown -> hidden: a close)
//   !clicked                         -> false  (no row pressed this frame -
//                                                covers a startup frame, where
//                                                nothing calls this at all,
//                                                as well as every other frame
//                                                nobody touched the row)
inline bool autoPresetTriggersOnWindowClick(bool clicked, bool wasShownBeforeClick) {
    return clicked && !wasShownBeforeClick;
}

// --- Preset bars: one key per valid preset, on the plugin's OWN window -----
//
// 0.99.0, from the owner's own words: "if I have multiple plugins open at
// the same time I want to select on the individual plugin to use its preset
// - so if I'm watching ADS-B I can click a button on POCSAG." Until this, the
// only place to press a preset was the DECODERS rail section
// (AppWindow::drawPluginPresets), so a user with ADS-B's map and POCSAG's
// decoder output both open had to go back to a rail row to send the radio to
// either one. What follows is the pure half of the fix: what a bar's keys
// look like, and the deferred-apply state machine that keeps a key press
// from reaching applyPluginPreset while AppWindow::drawPluginWindows is still
// iterating the very lists a preset's own apply rebuilds.

// THE SHARED VALIDITY FILTER. Every consumer of a plugin's preset table - the
// rail's buttons (drawPluginPresets), the auto-preset-on-start/on-show
// decision, the mute snapshot, the new bars, and the web status/apply paths
// - refuses the same third-party garbage the same way: a frequency that is
// not a frequency, NaN included, which is why this is a positive test rather
// than a negation. Kept here so several call sites cannot drift into several
// different opinions about what "a preset" means.
inline bool presetIsValid(const CascadePreset& ps) {
    return ps.frequencyHz > 0.0 && ps.frequencyHz < 1e12;
}

// THE SHARED CAP. A plugin is third-party code, and a list of buttons or bar
// keys thousands long is not a menu - it is a way for a buggy plugin to put a
// wall of controls on the panel, or to turn a per-frame frequency comparison
// into an unbounded loop. `cap` is AppWindow::kMaxPresetsPerPlugin, passed in
// rather than duplicated here: the constant belongs to the class that
// enforces it everywhere else, and this header must not fork a second copy
// of it.
inline std::uint32_t cappedPresetCount(std::uint32_t rawCount, std::uint32_t cap) {
    return rawCount > cap ? cap : rawCount;
}

// THE SHARED LABEL. The ABI promises `label` is NUL-terminated, but a plugin
// that fills every one of CASCADE_PRESET_LABEL_CHARS bytes with no
// terminator must not walk the host past the end of its own buffer -
// strnlen, not strlen, is what makes reading it safe. Falls back to
// `pluginName` when the plugin left the label empty, which is the common
// case: most presets letter nothing of their own and are meant to read as
// the plugin's name ("ADS-B" on the one preset an ADS-B decoder publishes).
inline std::string presetLabel(const char label[CASCADE_PRESET_LABEL_CHARS],
                                const std::string& pluginName) {
    const std::size_t len = strnlen(label, CASCADE_PRESET_LABEL_CHARS);
    return len == 0 ? pluginName : std::string(label, len);
}

// ONE PRESET AS THE PLUGIN'S OWN TABLE RETURNED IT, together with the RAW
// index get() was called with. The position of a preset within a FILTERED
// list (the ones presetIsValid let through) is NOT that index the moment any
// earlier one failed validation - and the raw index is exactly what a
// deferred apply must record, because it re-reads the preset through the
// plugin's own get() rather than trusting anything carried from the frame
// the key was pressed on. See AppWindow::validatedPresets, the one place
// that walks a plugin's count()/get() and builds this list.
struct IndexedPreset {
    std::uint32_t index = 0;
    CascadePreset preset{};
};

// ONE KEY OF A PRESET BAR, computed with no ImGui and no plugin ABI call:
// from a snapshot already taken (a plugin's cascade::core::MutePreset list -
// see AppWindow::rebuildMuteStates, which is the one place that snapshot is
// built, and the reason a bar never calls count()/get() itself) and the
// receiver's current position. `label` is already the bounded, name-
// falling-back text (the snapshot carries the RESULT of presetLabel, not the
// raw bytes) and `lit` is whether the receiver is already sitting on this
// preset - the same rule maybeAutoPreset uses to decide whether pressing it
// again would do anything.
struct PresetBarKey {
    std::uint32_t index = 0;
    std::string label;
    bool lit = false;
};

// A BAR HAS NOTHING TO DRAW when `presets` is empty: the return is an empty
// vector, and every caller's contract is "draw nothing and cost no vertical
// space" in that case, never a placeholder row.
inline std::vector<PresetBarKey> presetBarKeys(
    const std::vector<cascade::core::MutePreset>& presets, double deviceCentreHz,
    double vfoOffsetHz, double deviceRateHz) {
    std::vector<PresetBarKey> out;
    out.reserve(presets.size());
    for (const cascade::core::MutePreset& ps : presets) {
        PresetBarKey k;
        k.index = ps.index;
        k.label = ps.label;
        k.lit = detail::presetAlreadyTunedToFields(ps.frequencyHz, ps.bandwidthHz, ps.deviceCentre,
                                                    deviceCentreHz, vfoOffsetHz, deviceRateHz);
        out.push_back(std::move(k));
    }
    return out;
}

// --- The mid-iteration-rebuild hazard: record now, apply after the loop ----
//
// applyPluginPreset ends by calling refreshPluginRunner(), which rebuilds
// pluginUi_'s panels, instruments and images and, through ensureMapPage's own
// bookkeeping, the map page list too - the very containers
// AppWindow::drawPluginWindows is iterating when one of ITS windows draws a
// preset bar. 0.96.1 fixed a crash of exactly this shape in a different list
// (see gui/list_pick.hpp: "pick by index, apply after the loop" - a combo
// handler that rebuilt a list while still inside the loop that was drawing
// it). A bar's key press must do what that fix does: RECORD which plugin and
// which raw preset index were pressed, and let the actual apply happen once
// every window this frame has had its say.
//
// This is deliberately not a bare std::optional<PresetBarRequest> exposed to
// callers: record() and take() are the only operations, so a caller cannot
// read the pending request twice or forget to clear it. ONE request survives
// at most - a second press in the same frame (two different bars, or the
// same bar clicked twice by a very fast double-click) REPLACES whatever was
// pending rather than queuing it, because "the last thing you pressed" is
// the only sane answer to "what happens" when two presses landed in one
// frame nobody could have told apart anyway.
struct PresetBarRequest {
    std::string pluginKey;          // cascade::core::pluginKey() - the module file name
    std::uint32_t presetIndex = 0;  // the RAW index into the plugin's own table
};

class PendingPresetRequest {
public:
    void record(std::string pluginKey, std::uint32_t presetIndex) {
        pending_ = PresetBarRequest{std::move(pluginKey), presetIndex};
    }
    bool hasPending() const { return pending_.has_value(); }
    // Consumed ONCE: a second call in the same frame (there should never be
    // one, since only one safe point calls this) finds nothing, exactly as
    // if nothing had been pressed.
    std::optional<PresetBarRequest> take() {
        std::optional<PresetBarRequest> out = std::move(pending_);
        pending_.reset();
        return out;
    }

private:
    std::optional<PresetBarRequest> pending_;
};

// WHETHER A RESOLVED REQUEST IS SAFE TO HAND TO applyPluginPreset, asked at
// the safe point after the frame's window loops, never at the press itself.
// `pluginFound` is whether `pluginKey` still names a LOADED plugin with a
// preset table - false when the plugin was removed or unloaded between the
// press and this point, which a rescan mid-frame cannot do today but a
// module removed through the store while its window was open is exactly this
// case in miniature. `count` is that plugin's own answer to count(), read
// FRESH: the ABI promises a preset table cannot change without a rescan, but
// the index recorded when the key was pressed is only ever trusted against a
// bound read again right here, never against what count() answered back when
// the bar was drawn. `fetchedOk` is whether get(presetIndex, &ps) returned 1.
inline bool presetRequestStillValid(bool pluginFound, std::uint32_t count,
                                     std::uint32_t maxPresets, std::uint32_t presetIndex,
                                     bool fetchedOk, const CascadePreset& ps) {
    if (!pluginFound) { return false; }
    if (presetIndex >= count || presetIndex >= maxPresets) { return false; }
    if (!fetchedOk) { return false; }
    return presetIsValid(ps);
}

// --- The bar's own wrap: which row each key is drawn on --------------------
//
// THE WHOLE LAYOUT, DECIDED BEFORE ANYTHING IS DRAWN. The first version of the
// bar asked ImGui, key by key, "what is left on the current row"
// (GetContentRegionAvail().x) and called SameLine() when the next key fitted
// in the answer. That question cannot be asked where it was being asked:
// once a button has been submitted WITHOUT a SameLine() after it, ImGui has
// already moved the cursor to the start of the next line, so "what is left"
// is always the full width and every key always "fits". The rendered check
// showed exactly that (2026-09-17): FLEX's four keys on one row in a 570 px
// window, the third cut off by the frame and the fourth not on screen at
// all - while the arithmetic test was green, because the arithmetic was
// never what was wrong.
//
// So the draw code no longer asks as it goes. It measures every key, hands
// the widths here with the width of the row and the gap ImGui puts between
// two items on a line, and draws key i on the same line as key i-1 exactly
// when their rows are equal. What is tested is then what is drawn.
//
// A KEY WIDER THAN THE ROW still gets a row - its own - rather than being
// dropped: it belongs wherever the cursor is, however wide it turns out to
// be, so a bar can never wrap before it has drawn anything and leave a row
// with nothing on it. Exactly filling the row fits (<=, not <).
inline std::vector<std::size_t> presetBarRows(float rowWidthPx, float spacingPx,
                                              const std::vector<float>& keyWidthsPx) {
    std::vector<std::size_t> rows;
    rows.reserve(keyWidthsPx.size());
    std::size_t row = 0;
    float used = 0.0f;  // width taken on the current row, 0 = nothing on it yet
    for (const float w : keyWidthsPx) {
        if (used > 0.0f && used + spacingPx + w > rowWidthPx) {
            ++row;
            used = 0.0f;
        }
        used = (used > 0.0f) ? used + spacingPx + w : w;
        // A zero-width key must still count as "something on this row", or
        // the next key would be treated as a row's first and never wrap.
        if (used <= 0.0f) { used = 0.0001f; }
        rows.push_back(row);
    }
    return rows;
}

// --- Per-digit tuning: the tubes' own arithmetic, shared with the switches --
//
// The counter has ten cells, most significant first - 1 GHz down to 1 Hz -
// and the mouse wheel over one of them has always stepped the TUNED
// frequency by that digit's place value. 0.88.0 adds a toggle switch
// beneath every cell that does the same thing on a flick or a held repeat;
// this is that one piece of arithmetic, lifted out of
// AppWindow::drawFrequencyReadout so the wheel and the switches share it
// exactly rather than carrying two copies that could drift apart, and so a
// digit's place value and its zero clamp are each checkable without a live
// source or an open ImGui frame.
inline constexpr int kFreqDigitCells = 10;
inline constexpr double kFreqDigitPlaceHz[kFreqDigitCells] = {1e9, 1e8, 1e7, 1e6, 1e5,
                                                               1e4, 1e3, 1e2, 1e1, 1e0};

// cellIndex must be in [0, kFreqDigitCells) - the only caller is the counter's
// own fixed ten-cell loop, so this does not defend against anything wider.
inline double digitPlaceHz(int cellIndex) { return kFreqDigitPlaceHz[cellIndex]; }

// ONE STEP ON ONE DIGIT, clamped at 0 Hz - a tune may never ask the source
// for a negative centre (see drawFrequencyReadout's own comment on
// minTunedHz, which clamps a step further still, to the VFO offset, on top
// of this). up adds the digit's place value, !up subtracts it.
inline double stepDigit(double currentHz, int cellIndex, bool up) {
    const double delta = digitPlaceHz(cellIndex);
    return std::max(0.0, currentHz + (up ? delta : -delta));
}

// --- The tuner plate's own geometry ------------------------------------------
//
// 0.88.0: THE COUNTER IS A PLATE BOLTED ONTO THE FRONT OF THE DECK. The
// owner handed over a design reference - a 1950s-60s military frequency
// tuner: an olive-drab riveted plate carrying an engraved name plate, a
// power lamp and a MHz readout across its head, a black bezel holding ten
// Nixie tubes, a chrome toggle switch beneath every tube, and a footer line
// - and asked for it "bolted on to the front" of the deck, reproduced as
// faithfully as draw-list primitives allow. This is every measurement of
// that plate, in the deck's own reference units at scale 1, kept here with
// no ImGui dependency so a test can pin where a tube and its switch sit
// without an open frame.
//
// THE PLATE IS SIZED TO THE DECK, NOT THE DECK TO THE PLATE. The first cut
// scaled the reference down to the deck's 28-wide digit face and let the
// bar grow 69 units to hold the 207-tall result, with the volume dial and
// the window's minimum width shifted right for its 408 of width. The
// owner's words on that cut, both binding: it "need[s] to be smaller", and
// "we don't want to affect the size of the top bar - it's perfect the way
// we have it". So the bar stays 160 tall and the plate is compacted to fit:
// 364 wide, standing between the counter's two dividers with 12 units of
// brass clear on either side (kCounterDividerX in app_window.cpp is placed
// from this width), and 121 tall inside the bar.
//
// WHAT WAS SHRUNK, AND WHAT WAS NOT. The digit face keeps its size (the
// tube is 28 x 40, a hair under the reference's 3:4.4); every element of
// the reference is still here - rivets, name plate, lamp and readout,
// bezel, ten tubes, ten switches with collar, lever and ball, UP/DN
// stencils, footer - at the smallest tokens the look allows: a 12-unit name
// plate strip, a 30-unit switch area with a 14-unit collar and an 8-unit
// ball, stencils and footer at nine (nothing on this deck is lettered
// smaller), paddings of four to six. The ball now overlaps the stencil it
// points at, which is what the reference's own ball does too.
inline constexpr float kFreqCellW = 28.0f;   // one tube, and the switch beneath it
inline constexpr float kFreqTubeH = 40.0f;   // the digit face at its own size, in glass
inline constexpr float kFreqCellGap = 6.0f;  // the reference's 8, closed up for width
inline constexpr float kFreqTubeSwitchGap = 4.0f;  // the reference's 10, scaled
inline constexpr float kFreqSwitchH = 30.0f;       // the whole toggle switch area
inline constexpr float kFreqSwitchHalfH = kFreqSwitchH * 0.5f;  // UP half / DN half

// The plate's paddings and the rows inside it, top to bottom: the header
// (name plate and status cluster), a gap, the bezel, a gap, the footer line.
inline constexpr float kFreqPlatePadX = 10.0f;
inline constexpr float kFreqPlatePadTop = 5.0f;
inline constexpr float kFreqPlatePadBottom = 5.0f;
inline constexpr float kFreqPlateHeaderH = 12.0f;   // the name plate strip
inline constexpr float kFreqPlateHeaderGap = 4.0f;
inline constexpr float kFreqBezelPadX = 5.0f;
inline constexpr float kFreqBezelPadY = 4.0f;
inline constexpr float kFreqPlateFooterGap = 4.0f;
inline constexpr float kFreqPlateFooterH = 9.0f;
// The rivets: their centres this far in from each corner of the plate -
// inside the plate padding, so a rivet never lands on the name plate strip
// or the footer's first letter.
inline constexpr float kFreqPlateRivetInset = 5.0f;
inline constexpr float kFreqPlateRivetR = 2.0f;

inline constexpr float kFreqBezelW =
    kFreqDigitCells * kFreqCellW + (kFreqDigitCells - 1) * kFreqCellGap +
    kFreqBezelPadX * 2.0f;  // 280 + 54 + 10 = 344
inline constexpr float kFreqBezelH = kFreqBezelPadY + kFreqTubeH + kFreqTubeSwitchGap +
                                     kFreqSwitchH + kFreqBezelPadY;  // 4+40+4+30+4 = 82
inline constexpr float kFreqPlateW = kFreqBezelW + kFreqPlatePadX * 2.0f;  // 364
inline constexpr float kFreqPlateH = kFreqPlatePadTop + kFreqPlateHeaderH +
                                     kFreqPlateHeaderGap + kFreqBezelH +
                                     kFreqPlateFooterGap + kFreqPlateFooterH +
                                     kFreqPlatePadBottom;  // 5+12+4+82+4+9+5 = 121

// Where the bezel's own top-left sits inside the plate.
inline constexpr float kFreqBezelX = kFreqPlatePadX;
inline constexpr float kFreqBezelY =
    kFreqPlatePadTop + kFreqPlateHeaderH + kFreqPlateHeaderGap;  // 21

// A rectangle in screen pixels - deliberately not ImVec2: this header has no
// ImGui dependency, and none of its callers need one to check it.
struct FreqRect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// A CELL'S OWN LEFT EDGE, from the plate's top-left X. Shared by the tube and
// the switch beneath it, so the switch can never sit a pixel off the column
// its own tube is drawn in.
inline float freqCellLeftX(float plateTLx, int cellIndex, float scale) {
    return plateTLx + (kFreqBezelX + kFreqBezelPadX +
                       static_cast<float>(cellIndex) * (kFreqCellW + kFreqCellGap)) *
                          scale;
}

// ONE TUBE'S GLASS. plateTLx/plateTLy is the plate's own top-left in screen
// pixels (the point drawFrequencyReadout is handed), cellIndex in
// [0, kFreqDigitCells), scale the bar's current scale. The tube is the digit
// cell: the wheel over it tunes that digit and a click opens the typed
// editor, exactly as the drum apertures it replaces did.
inline FreqRect tubeRectForCell(float plateTLx, float plateTLy, int cellIndex, float scale) {
    const float x0 = freqCellLeftX(plateTLx, cellIndex, scale);
    const float y0 = plateTLy + (kFreqBezelY + kFreqBezelPadY) * scale;
    return FreqRect{x0, y0, x0 + kFreqCellW * scale, y0 + kFreqTubeH * scale};
}

// ONE SWITCH HALF'S RECTANGLE - the SAME one for its InvisibleButton and for
// what is drawn inside it, the "hit area is the drawn area" contract an
// earlier cut of the digit keys got wrong once: a live click test on the
// built app found a key's own ink was not where its hit box actually was,
// because the button and the shape beside it were two separate
// calculations. upperHalf selects the switch's top half (true - a flick UP,
// stepping the digit up) or bottom half (false - DN). The two halves tile the
// whole kFreqSwitchH area with no gap and no overlap.
// AppWindow::drawFrequencyReadout calls this once per half to place
// ImGui::SetCursorScreenPos/InvisibleButton, and the drawing that follows
// reads back ImGui::GetItemRectMin()/Max() of that SAME button rather than
// recomputing anything.
inline FreqRect switchRectForCell(float plateTLx, float plateTLy, int cellIndex, bool upperHalf,
                                  float scale) {
    const float x0 = freqCellLeftX(plateTLx, cellIndex, scale);
    const float areaTopY =
        plateTLy + (kFreqBezelY + kFreqBezelPadY + kFreqTubeH + kFreqTubeSwitchGap) * scale;
    const float y0 = upperHalf ? areaTopY : areaTopY + kFreqSwitchHalfH * scale;
    return FreqRect{x0, y0, x0 + kFreqCellW * scale, y0 + kFreqSwitchHalfH * scale};
}

// --- HOW A DIGIT CELL IS PAINTED -------------------------------------------
//
// WHY THIS IS A SETTING AT ALL. GitHub issue #1: "Would be nice to be able to
// swap the frequency display for easier to read display? Maybe a neon effect
// or something that stands out." The Nixie plate is the deck's own face and
// stays the default, so nobody's application changes under them; the two new
// styles trade the reference's glass, mesh and ghost cathode for a figure
// that is simply bigger and higher in contrast.
//
// ONLY THE PAINT INSIDE A TUBE RECTANGLE CHANGES. The rectangles above are
// the same for every style, so every gesture the counter has - the wheel over
// a tube, the typed editor, both switch halves, the MHz readout and the RCVR
// lamp - is untouched by the choice. The name plate, the screws, the bezel
// and the footer are the plate, not the cell, and they do not change either.
enum class TunerStyle {
    Nixie = 0,  // the plate as drawn since 0.88.0 - the default, unchanged
    Neon,       // a large glowing figure on a near-black cell
    Plain,      // a large plain white figure on black, no glow at all
};

// THE NAME IS WHAT THE CONFIG FILE CARRIES, so it is the name that decides.
// An unknown or empty value is the DEFAULT, never a refusal: the file is
// user-editable and a typo must leave the application looking like itself
// rather than drawing nothing. Matching is exact and lower-case - the names
// are written by this application, not typed by a user.
inline TunerStyle tunerStyleFromName(const std::string& name) {
    if (name == "neon") { return TunerStyle::Neon; }
    if (name == "plain") { return TunerStyle::Plain; }
    return TunerStyle::Nixie;
}

inline const char* tunerStyleName(TunerStyle style) {
    switch (style) {
        case TunerStyle::Neon: return "neon";
        case TunerStyle::Plain: return "plain";
        case TunerStyle::Nixie: break;
    }
    return "nixie";
}

// The three names in the order the menu offers them, so the picker and the
// config vocabulary cannot drift apart.
inline constexpr int kTunerStyleCount = 3;
inline const char* kTunerStyleNames[kTunerStyleCount] = {"nixie", "neon", "plain"};
// What the picker LABELS them. Separate from the stored names because the
// stored name is a token in a file and the label is prose on a bench panel.
inline const char* kTunerStyleLabels[kTunerStyleCount] = {"Nixie tubes", "Neon", "Plain"};

// EVERY NUMBER ONE CELL IS PAINTED WITH, for one style. Kept here, with no
// ImGui dependency, so a later edit to the drawing cannot silently change a
// style's look: tests/test_tune_control.cpp pins all three sets, and the
// painters in app_window.cpp read every field from here rather than carrying
// literals of their own. Colours are 0xRRGGBB with a separate 8-bit alpha,
// because this header has no ImU32 and needs none.
struct TunerCellPaint {
    // The digit's pixel size as a fraction of the tube's height. 0.635 is
    // the Nixie plate's own figure (0.635 of a 40-unit tube = 25.4 px at
    // scale 1); the flat styles go much larger, which is the whole point of
    // the request - the figure, not the furniture, is what is read.
    float digitFrac = 0.635f;
    // Offset translucent copies of the glyph, drawn as rings around it to
    // stand in for a blur ImDrawList cannot do. 0 means no glow of any kind.
    int glowLayers = 0;
    // The outermost ring's offset in plate units (scaled by the bar's scale
    // at the call site). Inner rings step in evenly from here.
    float glowSpread = 0.0f;
    // How far out the soft disc behind the figure reaches, in plate units.
    // 0 means no disc.
    float haloUnits = 0.0f;
    unsigned digitRgb = 0xffb347u;     // the lit figure
    unsigned char digitAlpha = 255u;
    unsigned char dimAlpha = 96u;      // a leading zero, which carries no value
    // THE TIGHTEST GLOW RING - the one nearest the glyph, and so the brightest
    // one. Outer rings are this colour at a lower alpha. The Nixie's two
    // further shadows (the wider ring and the soft disc) are the reference's
    // own and stay with the painter; only their REACH (glowSpread, haloUnits)
    // is a parameter, because that is the part a style changes.
    unsigned glowRgb = 0xff8a1fu;
    unsigned char glowAlpha = 70u;
    unsigned cellRgb = 0x050403u;      // the cell's ground
    // Whether the cell carries the reference's glass: the rounded envelope,
    // the radial interior, the two mesh line sets and the ghost "8" cathode.
    // False draws a flat cell, which is what makes a figure at this contrast
    // legible at a glance.
    bool glassFurniture = true;
};

inline TunerCellPaint tunerCellPaint(TunerStyle style) {
    TunerCellPaint p;
    switch (style) {
        case TunerStyle::Nixie:
            // The values the plate has drawn with since 0.88.0, moved here
            // unchanged - the default must stay pixel-identical, so these are
            // transcriptions, not choices.
            p.digitFrac = 0.635f;
            p.glowLayers = 2;
            p.glowSpread = 3.0f;
            p.haloUnits = 10.0f;
            p.digitRgb = 0xffb347u;
            p.digitAlpha = 255u;
            p.dimAlpha = 96u;
            p.glowRgb = 0xff8a1fu;  // "0 0 6px #ff8a1f", the tightest shadow
            p.glowAlpha = 70u;
            p.cellRgb = 0x050403u;  // the glass gradient's own outer colour
            p.glassFurniture = true;
            break;
        case TunerStyle::Neon:
            // CYAN, NOT AMBER. The amber IS the Nixie, and a setting whose
            // two positions look like the same lamp at two brightnesses is
            // not a setting anybody can see working. Cyan on near-black is
            // also the highest-contrast pairing available here, which is
            // what the request actually asked for.
            p.digitFrac = 0.84f;
            p.glowLayers = 3;
            p.glowSpread = 4.5f;
            p.haloUnits = 7.0f;
            p.digitRgb = 0xd6feffu;  // the tube's own near-white core
            p.digitAlpha = 255u;
            p.dimAlpha = 70u;
            p.glowRgb = 0x00d0ffu;
            p.glowAlpha = 90u;
            p.cellRgb = 0x04070au;
            p.glassFurniture = false;
            break;
        case TunerStyle::Plain:
            // NO DECORATION AT ALL. White on black at the same size as the
            // neon figure: the reading for somebody who wants the number and
            // nothing else, and the one style that cannot be accused of
            // costing contrast for atmosphere.
            p.digitFrac = 0.84f;
            p.glowLayers = 0;
            p.glowSpread = 0.0f;
            p.haloUnits = 0.0f;
            p.digitRgb = 0xffffffu;
            p.digitAlpha = 255u;
            p.dimAlpha = 80u;
            p.glowRgb = 0xffffffu;
            p.glowAlpha = 0u;
            p.cellRgb = 0x000000u;
            p.glassFurniture = false;
            break;
    }
    return p;
}

// --- The deck's fixed cluster, and the two meters at the bar's right --------
//
// kDeckCoreW is the width of the fixed cluster on the top bar - transport
// button through the volume dial - in the deck's reference units at scale 1.
// It is the number the bar shrinks against on a narrow window and the number
// the meters must clear on a wide one; app_window.cpp reads it as kCoreW and
// places the dial from the same figure. Kept here, with no ImGui dependency,
// so the meters rule below can be checked against it without an open frame.
// 958 with the tuning knob on the deck; 888 since 0.90.0 removed it.
inline constexpr float kDeckCoreW = 888.0f;

// The narrowest client area run() lets the OS window reach. The bar's floor
// scale (kBarMinScale in app_window.cpp) is a promise about the volume dial
// that only holds if the window cannot go narrower than the dial needs, and
// the static_assert beside that scale ties the two together. 694 with the
// tuning knob on the deck; 624 since 0.90.0 removed it.
inline constexpr int kDeckMinWindowW = 624;

// THE BAR AT FIRST LAUNCH. run() creates a 1280 x 720 window and then takes
// the caption off its style (win_frame), which leaves a client area of
// 1282 x 745 (the diagnostic log's "frame: caption removed; ... client
// 1282x745"); the cabinet keeps a 22-unit rail (kRailMinMargin - the
// fraction-of-height margin comes out smaller than that at this size) plus
// the 3 px its bevel takes, each side, before the bar is drawn. So the bar a
// fresh install opens with is 1232 wide, and the meters rule is asked to
// pass at exactly that width: the owner's complaint on 0.89.0 was that the
// SAMPLE RATE and FRAME TIME meters were not on the deck until the window
// was widened or maximised, because the rule then wanted 1320.
inline constexpr float kFirstLaunchClientW = 1282.0f;
inline constexpr float kFirstLaunchBodyInset = 22.0f + 3.0f;
inline constexpr float kFirstLaunchBarW = kFirstLaunchClientW - 2.0f * kFirstLaunchBodyInset;

// The two bench meters, SAMPLE RATE and FRAME TIME, in bar pixels: they are
// pinned to the bar's right edge and do not scale, so these are screen
// pixels as well as reference units.
inline constexpr float kMeterW = 126.0f;
inline constexpr float kMeterGap = 16.0f;           // brass between the two meters
inline constexpr float kMeterRightMargin = 34.0f;   // the second meter to the bar's right edge
inline constexpr float kMeterCoreClearance = 12.0f; // the fixed cluster's end to the first meter

// Where each meter's left edge sits, measured from the bar's own left edge.
inline constexpr float meter2XOnBar(float barW) { return barW - kMeterRightMargin - kMeterW; }
inline constexpr float meter1XOnBar(float barW) { return meter2XOnBar(barW) - kMeterGap - kMeterW; }

// WHETHER THE METERS ARE DRAWN AT ALL. They are dropped on a narrow window
// rather than allowed to slide left into the volume dial (they are the least
// load-bearing things on the bar - both figures are also in the status
// column), and the rule is exactly "the first meter clears the fixed
// cluster by kMeterCoreClearance": nothing more, because the slack the old
// rule carried (110 units beyond the two meters) is what kept them off the
// deck at the default window.
inline constexpr bool metersFitOnBar(float barW, float coreW) {
    return meter1XOnBar(barW) >= coreW + kMeterCoreClearance;
}

// THE MUTE BANNER'S STRIP. The banner takes the bar's open middle - from the
// fixed cluster to the first meter, or to the bar's right edge when the
// meters are not drawn - when that strip is at least kMuteBannerMinW wide,
// and the strip under the counter otherwise. Both are inside the bar and
// neither can reach the dial or the meters, which is the whole point: a
// warning that overlaps a control is a warning the user cannot act on.
inline constexpr float kMuteBannerMinW = 220.0f;
inline constexpr float kMuteBannerEdgeClearance = 12.0f;
inline constexpr float muteBannerMiddleW(float barW, float coreW) {
    const float from = coreW + kMuteBannerEdgeClearance;
    const float to = metersFitOnBar(barW, coreW) ? meter1XOnBar(barW) - kMeterGap
                                                 : barW - kMuteBannerEdgeClearance;
    return to - from;
}
inline constexpr bool muteBannerTakesTheMiddle(float barW, float coreW) {
    return muteBannerMiddleW(barW, coreW) >= kMuteBannerMinW;
}

// --- WHAT A GAIN READS AS, and it is not always decibels --------------------
//
// THE DEFECT THIS EXISTS FOR. Every consumer of source::GainInfo lettered its
// number "%.1f dB" - the Source section's sliders, the RECEIVER card's
// "TUNER 30 dB +3 more", the scope deck's GAIN knob, and the browser's own
// gain row - because until the native Airspy R2/Mini every gain in FoxSDR
// really was decibels. The Airspy's five are register steps and table
// indices (source/device_source.hpp's GainUnit says why), so that panel
// showed "LNA 7.0 dB" for step 7: a wrong number carrying a wrong unit,
// which is worse than either alone because it reads as a measurement.
//
// Kept here, pure and with no ImGui, for the same reason as the rest of this
// header: four call sites in two translation units have to agree about how a
// gain is lettered, and a test can pin both units without a radio on the desk
// - which matters more than usual, since there is no Airspy on this bench and
// these functions are the only proof the Steps case is rendered at all.
//
// The wire word is here too so the browser and the desktop cannot drift: the
// JSON's "unit" field is this string.
inline const char* gainUnitWire(cascade::source::GainUnit unit) {
    return unit == cascade::source::GainUnit::Steps ? "step" : "dB";
}

// The ImGui slider's own format string. A step is a whole number and takes no
// unit; a decibel keeps the tenth it has always shown, because these radios
// quantise onto tenths and the slider reports the driver's readback.
inline const char* gainSliderFormat(cascade::source::GainUnit unit) {
    return unit == cascade::source::GainUnit::Steps ? "%.0f" : "%.1f dB";
}

// THE VALUE ALONE, as the card and the knob print it: "30 dB", or "7". Whole
// numbers in both cases - this is the glanceable readout, not the control.
inline std::string formatGainValue(double value, cascade::source::GainUnit unit) {
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  unit == cascade::source::GainUnit::Steps ? "%.0f" : "%.0f dB", value);
    return buf;
}

// NAME AND VALUE: "TUNER 30 dB", "LNA 7". One space, and nothing else - the
// RECEIVER card appends its own "+3 more" after this.
inline std::string formatGain(const std::string& name, double value,
                              cascade::source::GainUnit unit) {
    return name + " " + formatGainValue(value, unit);
}

// --- The Source section's device scan: never while a radio is open ---------
//
// THE 0.90.0 FIELD FAULT (crash store, 2026-09-09, a NESDR SMArt v5 on
// Windows 10.0.26200; absorbed by the vendor-call guard, so the process
// survived and the log is complete). The radio had streamed at 2.4 MS/s for
// two minutes. Then the user opened the Source section for the first time
// that session, which ran the device scan: UHD's banner appears in the log at
// 23:27:11 (the child process loading every vendor module), and twelve
// seconds later the next control call - a sample-rate change - died in
// ntdll's RtlEnterCriticalSection on a critical section libusb had already
// freed, inside rtlsdr.dll, under our own setSampleRateHz.
//
// WHAT THE SCAN DOES TO A STREAMING DONGLE. SoapyRTLSDR's find routine calls
// rtlsdr_get_device_usb_strings on every dongle (a NEW libusb context, open,
// read the strings, close) and then probes each one with rtlsdr_open +
// rtlsdr_close to say "available" - and rtlsdr_open runs the full demodulator
// reset and tuner initialisation. FoxSDR has refused the IN-PROCESS walk
// while a radio is open since 0.62.1 (SoapySource::enumerateInProcess,
// adjudicated fix #1), but the normal scan runs in a child process, whose
// probe opens and resets the same dongle from outside, with our stream live
// on it. The child is a different process; the dongle is the same dongle.
//
// So the scan is DEFERRED while a device is open - the app's own (deviceOpen:
// a SoapySource installed in the pipeline, or an open resolving on its
// worker) - and while another scan is already running. The section keeps the
// list it has, the Refresh key says why it is disabled, and the next draw
// after the radio closes scans as it always did. Pure, so the gate is
// checkable without a device, a pipeline or an ImGui frame; every caller of
// AppWindow::scanSoapy() (the combo's lazy first scan, Refresh, and the web
// interface's scanDevices) goes through it.
inline bool deviceScanAllowed(bool deviceOpen, bool scanPending, bool openPending) {
    return !deviceOpen && !scanPending && !openPending;
}

// --- Reopening a radio whose driver faulted, once ---------------------------
//
// The same field report, the other half. The fault was absorbed, the device
// was condemned (SoapySource's dead-device policy: never call a driver that
// has faulted), and the radio stayed dead until FoxSDR was restarted - with
// the deck showing FAIL and the user's session otherwise intact. One
// automatic reopen is worth attempting: the driver's own object is what is
// condemned, and a fresh open through the same async path either brings the
// radio back or fails in the ordinary way, with the ordinary message.
//
// The conditions, each of which is load-bearing:
//   - deadByAbsorbedFault: the device is dead because a vendor call FAULTED
//     on our own call frame and the guard absorbed it. Not any other kind of
//     dead.
//   - !driverAbandoned: a WEDGED driver - an escape-path call that never came
//     back, or a driver lock that never answered - has one of this process's
//     threads still parked inside the module. A reopen would put a second
//     thread in there beside it, which is the 0.62.0 crash class; that device
//     stays dead and the message ("restart FoxSDR to use this radio again")
//     stands. SoapySource::deadReason() tells the two apart.
//   - !openPending && !scanPending: nothing else may be inside the driver
//     stack, and a reopen already in flight must not be doubled.
//   - the last attempt was kSoapyReopenHoldoffSec ago or more (lastAttemptSec
//     < 0 means never): ONE attempt, not a retry loop against a radio that is
//     really gone. A reopen that faults again condemns its own device the
//     same way, and this holds the next attempt off for a minute.
inline constexpr double kSoapyReopenHoldoffSec = 60.0;

inline bool autoReopenDue(bool deadByAbsorbedFault, bool driverAbandoned, bool openPending,
                          bool scanPending, double nowSec, double lastAttemptSec) {
    if (!deadByAbsorbedFault || driverAbandoned) { return false; }
    if (openPending || scanPending) { return false; }
    if (lastAttemptSec >= 0.0 && nowSec - lastAttemptSec < kSoapyReopenHoldoffSec) {
        return false;
    }
    return true;
}

// --- PREFER THE NATIVE DRIVER, AUTOMATICALLY --------------------------------
//
// THE USER HAS TO DO NOTHING. From 0.91.0 FoxSDR has its own RTL-SDR and
// HackRF drivers, from 0.92.0 an Airspy R2/Mini and an Airspy HF+, and from
// 0.93.0 an SDRplay RSP, a Mirics MSi2500 and an RX888 mk2 - our USB
// transport, our reader thread, our enumeration (see src/usb/usb_device.hpp
// for why: every crash report this product received from a USB radio in its
// first month landed inside somebody else's libusb, on a thread we did not
// create, behind a vendor module we could not fix).
// A user whose config says "driver=rtlsdr" saved that before any of it
// existed, and nobody is going to reopen the Source section to switch over.
//
// So the decision is made for them: when the saved source is a SoapySDR
// device whose driver key names a radio we now drive ourselves, and a native
// row is present for the SAME DONGLE, the native driver opens instead. The
// same rule fires when the user clicks a Soapy rtlsdr row in the dropdown
// while a native row exists for that serial - picking the radio should not
// also be picking which of two code paths reaches it.
//
// WHAT "THE SAME DONGLE" MEANS, and why it is not simply "any RTL-SDR". With
// two dongles plugged in, a config that says serial=00000002 must not be
// silently answered with serial=00000001: that is a different antenna on a
// different band, and the user would have no way to tell what happened. So a
// saved serial must match. A saved args string with NO serial (the bare
// "driver=rtlsdr" every hand-written config and most Soapy enumerations
// produce) names no particular dongle, so the first native row of that driver
// is the honest answer to it.
//
// Serial matching is case-insensitive, and a SUFFIX match counts - the long
// form of a HackRF serial is 32 hex digits and every tool that prints it
// prints the tail, so HackRfSource::open takes a suffix and this must agree
// with it or the rule would point at a device the driver then refuses. An
// Airspy HF+ needs one step more: Windows reports its serial as the whole USB
// string "AIRSPYHF SN:0123456789ABCDEF" while every other tool and every user
// quotes the sixteen hex digits, so both sides go through the driver's own
// normalisedSerial first - exactly as AirspyHfSource::open does.
//
// SEVEN DRIVER KEYS NOW, and "airspy" and "airspyhf" are matched WHOLE rather
// than by prefix: one is the prefix of the other, they are different USB ids
// and different radios, and answering a saved HF+ with an R2 would hand the
// user a receiver that cannot reach a single frequency they were listening to.
//
// Returns nothing when the rule does not apply, which is the common case and
// is not a failure: a B200, a LimeSDR, a saved generator, an RTL-SDR whose
// dongle is unplugged or is still on the DVB-T driver.
inline bool nativeSerialMatches(const std::string& nativeArgs, const std::string& wantSerial,
                                const std::string& driver) {
    std::string have = cascade::source::argValue(nativeArgs, "serial");
    std::string want = wantSerial;
    if (driver == "airspyhf") {
        have = cascade::source::normalisedSerial(have);
        want = cascade::source::normalisedSerial(want);
    }
    if (have.empty() || want.empty()) { return false; }
    const auto lower = [](std::string t) {
        for (char& c : t) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return t;
    };
    const std::string a = lower(have);
    const std::string b = lower(want);
    if (a == b) { return true; }
    // Suffix either way: the saved string may be the long form and the
    // enumerated one the short, or the other way round.
    if (a.size() > b.size()) { return a.compare(a.size() - b.size(), b.size(), b) == 0; }
    return b.compare(b.size() - a.size(), a.size(), a) == 0;
}

// WHAT A SoapySDR DRIVER NAME IS CALLED IN OUR OWN LIST, and for three of
// them the two words are not the same word.
//
// The rule used to compare the saved `driver=` straight against the native
// driver key, which worked only because SoapyRTLSDR, SoapyHackRF and the two
// Airspy modules happen to publish exactly the names we chose. The three
// drivers added in 0.93.0 do not:
//
//   driver=sdrplay  is SoapySDRPlay3   -> our "sdrplay"   (the same word, and
//                                         it is here so the list is complete)
//   driver=miri     is SoapyMiri       -> our "mirisdr"
//   driver=sddc     is SoapySDDC       -> our "rx888"
//
// The module's spelling is the one in the user's saved config and is not ours
// to choose, so the translation lives here, once, with the module that
// publishes each name written beside it. An unknown name maps to nothing,
// which is what leaves a B200 or a LimeSDR alone.
//
// The Pluto is deliberately absent. SoapyPlutoSDR addresses a board by URI
// rather than by serial, so there is no "is this the same physical radio"
// comparison to make - and with no USB serial on either side, answering a
// saved Soapy Pluto with our row would be a guess dressed up as a match.
inline std::string nativeKeyForSoapyDriver(const std::string& soapyDriver) {
    if (soapyDriver == "rtlsdr" || soapyDriver == "hackrf" || soapyDriver == "airspy" ||
        soapyDriver == "airspyhf" || soapyDriver == "sdrplay") {
        return soapyDriver;
    }
    if (soapyDriver == "miri") { return "mirisdr"; }
    if (soapyDriver == "sddc") { return "rx888"; }
    return std::string();
}

inline std::optional<cascade::source::NativeDeviceInfo> preferNativeFor(
    const std::string& savedKind, const std::string& savedArgs,
    const std::vector<cascade::source::NativeDeviceInfo>& native) {
    // Only a SAVED SOAPY DEVICE is upgraded. A saved native device is already
    // native, and the generator and the IQ file are not radios.
    if (savedKind != "soapy") { return std::nullopt; }
    std::string soapyDriver = cascade::source::argValue(savedArgs, "driver");
    for (char& c : soapyDriver) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const std::string driver = nativeKeyForSoapyDriver(soapyDriver);
    if (driver.empty()) { return std::nullopt; }
    const std::string serial = cascade::source::argValue(savedArgs, "serial");
    const cascade::source::NativeDeviceInfo* first = nullptr;
    for (const cascade::source::NativeDeviceInfo& d : native) {
        if (d.driver != driver) { continue; }
        if (first == nullptr) { first = &d; }
        if (!serial.empty() && nativeSerialMatches(d.args, serial, driver)) { return d; }
    }
    // No serial saved: the first row of that driver IS the device the saved
    // args named, because the saved args named no particular one.
    if (serial.empty() && first != nullptr) { return *first; }
    return std::nullopt;
}

// THE ONE MESSAGE THE NATIVE OPEN IS ALLOWED TO GIVE UP ON, and it is a real
// dongle, not a hypothetical: an E4000 or FC0012/13 tuner. RtlSdrSource
// supports the R820T and the R828D and says so in as many words when it meets
// anything else. A user on one of those must not be left with a dead Source
// section when the SoapySDR path they were already using works perfectly -
// so the prefer-native open falls back to it, and says why.
//
// Matched on the distinctive middle of RtlSdrSource's own sentence rather
// than the whole of it, because the whole of it is wrapped across two source
// lines and a re-wrap must not silently turn the fallback off. A test pins
// the two together.
inline constexpr const char* kTunerUnsupportedMarker = "tuner is not one this driver supports";

inline bool nativeOpenShouldFallBack(const std::string& error) {
    return error.find(kTunerUnsupportedMarker) != std::string::npos;
}

// --- The saved radio outlives a restore that could not open it -------------

// ONE PLACE DECIDES WHICH SOURCE KINDS ARE NATIVE DRIVERS. There are eight of
// them, read by the config restore, the web remote's device match,
// makeDeviceSource's construction and the remembered-source rule below, and a
// list of string literals copied per call site is one chance per copy for an
// Airspy to become quietly unrestorable while everything else keeps working.
// It sits here rather than in app_window.cpp's anonymous namespace because
// the decisions in this file need it too - and because a rule kept in a .cpp
// is a rule no test can reach, which is the whole reason this header exists.
//
// "NATIVE" MEANS "FoxSDR's OWN DRIVER", NOT "OVER OUR WINUSB TRANSPORT", and
// the last two rows are why the distinction has to be written down. An RSP is
// reached through the vendor API the user installed and a Pluto over TCP to
// the board's own daemon - neither touches src/usb at all - but both are code
// this product wrote and can be held responsible for, both carry their own
// sourceKind, and both restore, save and switch through exactly the same
// paths as the six USB ones. Anything that needs "does this go through
// WinUSB" is asking a different question and must not use this list.
inline bool isNativeSourceKind(const std::string& kind) {
    return kind == "rtlsdr" || kind == "hackrf" || kind == "airspy" || kind == "airspyhf" ||
           kind == "sdrplay" || kind == "mirisdr" || kind == "rx888" || kind == "pluto";
}

// WHAT TO CALL A RECORDING ON SCREEN: its own name, not the path it lives at.
//
// The Source section names a saved-but-not-open source in a combo preview one
// column wide and in one sentence beneath it, and an I/Q capture lives several
// folders down a drive that may not be plugged in. The folders are in
// sourceError_ for the person reading it; the name is what identifies the file
// at a glance.
//
// BOTH SEPARATORS, because the path came out of a config file that a user may
// have written by hand or carried from another machine, and a Windows path
// with forward slashes opens perfectly well. A path that ends in a separator
// names no file at all, so the whole string is given back rather than an empty
// caption: an odd-looking label says more than a blank one.
inline std::string fileNameOf(const std::string& path) {
    const std::size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos) { return path; }
    const std::string name = path.substr(cut + 1);
    return name.empty() ? path : name;
}

// THE SOURCE THE CONFIG NAMES, HELD OVER A SESSION THAT COULD NOT OPEN IT.
//
// The startup restore falls back to the signal generator when the saved radio
// does not open - the dongle unplugged, in use by another program, or a
// second copy of FoxSDR holding it, which is the case that found this - and
// that fallback is right: the session has to run on something. What was wrong
// is what the exit save then wrote. The live source is the generator, so the
// config went out as "siggen" with both args slots empty, and ONE session
// with the dongle out of its socket was enough for FoxSDR to forget the radio
// permanently: the next start had nothing to try, and the user had to find
// their receiver in the Source section again with nothing anywhere saying
// why it had gone.
//
// So a failed restore keeps these values and the save writes them in place of
// what is live (see sourceToSave). BOTH args slots travel, because they are
// different grammars for different openers and the next start needs both -
// soapyArgs is what the prefer-native rule reads, nativeArgs is what a native
// driver opens (see AppConfig::nativeArgs). The rate travels with them: the
// generator runs at a fixed 2 MS/s, and writing THAT back as the radio's rate
// would bring the dongle up next time at a rate the user never chose.
//
// The antenna and the bias tee need nothing here - the restore seeds those
// mirrors from the config before it attempts the open, so a failed open
// leaves them holding exactly what the file carried.
//
// AND THE SAME IS TRUE OF A SAVED I/Q FILE, which 0.93.0 left behind and
// 0.94.1 finishes. A recording is lost exactly the way a dongle is - an
// external drive unplugged, a folder renamed, a capture moved or deleted -
// and the file branch of the restore fell back to the generator with the path
// dropped as well: sourceKind went out as "siggen" and iqFilePath EMPTY,
// because that mirror is only filled by an open that succeeded. One session
// with the drive out and FoxSDR could no longer say which file it had been
// playing, let alone try it again. So a file travels here too, by its path,
// on exactly the terms a radio does.
struct RememberedSource {
    std::string kind;  // empty: nothing to remember, the live source is the truth
    std::string soapyArgs;
    std::string nativeArgs;
    std::string filePath;  // only ever set for kind "file"
    double sampleRateHz = 0.0;

    bool valid() const { return !kind.empty(); }
};

// WHAT A FAILED RESTORE IS ALLOWED TO REMEMBER: a source that names a
// PARTICULAR thing to open. A radio with args for its own family, or a file
// with a path. The generator names nothing (it is what the session already
// fell back to), and a kind with no args or no path of its own names no
// particular device or recording, so there is nothing there worth carrying
// into the next launch. Anything else returns an empty RememberedSource,
// which sourceToSave reads as "save what is live", i.e. exactly today's
// behaviour.
//
// BOTH ARGS SLOTS TRAVEL WITH A REMEMBERED FILE as well, untouched. They
// belong to a radio the user opened at some point before choosing the file,
// and a session that could not find the recording has learned nothing about
// the radio - blanking them would make a missing file take the dongle with
// it, which is the very bug this rule exists to stop, one source along.
inline RememberedSource rememberedSourceAfterFailedOpen(const std::string& savedKind,
                                                        const std::string& savedSoapyArgs,
                                                        const std::string& savedNativeArgs,
                                                        const std::string& savedFilePath,
                                                        double savedSampleRateHz) {
    RememberedSource keep;
    const bool soapy = (savedKind == "soapy") && !savedSoapyArgs.empty();
    const bool native = isNativeSourceKind(savedKind) && !savedNativeArgs.empty();
    // A PATH IS NOT ENOUGH ON ITS OWN: every config that ever played a file
    // carries iqFilePath so the box comes back filled in, and only a config
    // whose KIND is "file" was actually listening to it.
    const bool file = (savedKind == "file") && !savedFilePath.empty();
    if (!soapy && !native && !file) { return keep; }
    keep.kind = savedKind;
    keep.soapyArgs = savedSoapyArgs;
    keep.nativeArgs = savedNativeArgs;
    if (file) { keep.filePath = savedFilePath; }
    // A rate of zero is a config that never recorded one; the restore's own
    // open() treats it the same way, so do not write it back as if chosen.
    keep.sampleRateHz = savedSampleRateHz > 0.0 ? savedSampleRateHz : 0.0;
    return keep;
}

// WHICH SOURCE THE SAVE NAMES. The remembered source - a radio or an I/Q file
// - wins ONLY while the generator is what is running, which is the fallback
// state a failed restore leaves behind and nothing else. The moment any real
// source is installed (another radio opened, an I/Q file opened) the live
// values are the truth and are written, and a user who deliberately picks the
// generator has their remembered source dropped at the point they pick it,
// not here: a deliberate choice must still overwrite, exactly as it always
// has.
//
// The gate on the live kind is belt and braces for that clearing - a saved
// radio silently outliving an open one would be a far worse bug than the one
// this fixes.
struct SavedSource {
    std::string kind;
    std::string soapyArgs;
    std::string nativeArgs;
    std::string filePath;
    double sampleRateHz = 0.0;
};

inline SavedSource sourceToSave(const std::string& liveKind, const std::string& liveSoapyArgs,
                                const std::string& liveNativeArgs,
                                const std::string& liveFilePath, double liveSampleRateHz,
                                const RememberedSource& remembered) {
    SavedSource out;
    out.kind = liveKind;
    out.soapyArgs = liveSoapyArgs;
    out.nativeArgs = liveNativeArgs;
    out.filePath = liveFilePath;
    out.sampleRateHz = liveSampleRateHz;
    if (remembered.valid() && liveKind == "siggen") {
        out.kind = remembered.kind;
        out.soapyArgs = remembered.soapyArgs;
        out.nativeArgs = remembered.nativeArgs;
        // THE PATH ONLY WHEN THERE IS ONE TO REMEMBER, which is the same
        // guard the rate has and for the same reason. A remembered RADIO
        // carries no path, and the live iqFilePath is the box's own memory of
        // the last recording played - it describes no source choice, so a
        // failed radio restore must not blank it on its way past.
        if (!remembered.filePath.empty()) { out.filePath = remembered.filePath; }
        if (remembered.sampleRateHz > 0.0) { out.sampleRateHz = remembered.sampleRateHz; }
    }
    return out;
}

}  // namespace cascade::gui
