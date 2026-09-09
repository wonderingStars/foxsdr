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

}  // namespace cascade::gui
