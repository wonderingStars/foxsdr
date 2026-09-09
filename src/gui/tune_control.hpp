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
#include <string>
#include <vector>

#include "core/plugin_abi.h"

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

inline bool presetAlreadyTunedTo(const CascadePreset& ps, double deviceCentreHz,
                                  double vfoOffsetHz, double deviceRateHz) {
    if ((ps.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u) {
        // Zero rate (no source open, or a source that has not answered yet)
        // collapses the tolerance to zero rather than dividing by anything -
        // "inside" then means exactly on the mark, which is the safe answer
        // when there is no band to speak of at all.
        const double toleranceHz = 0.5 * deviceRateHz;
        return std::fabs(deviceCentreHz - ps.frequencyHz) <= toleranceHz;
    }
    const double toleranceHz = std::max(0.5 * ps.bandwidthHz, 5000.0);
    const double tunedHz = deviceCentreHz + vfoOffsetHz;
    return std::fabs(tunedHz - ps.frequencyHz) <= toleranceHz;
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

}  // namespace cascade::gui
