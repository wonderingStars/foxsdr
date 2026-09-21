// fm_mpx_plan.hpp - what is in the FM multiplex, and where to write it on a
// spectrum.
//
// THE REQUEST (owner, 2026-09-21, with a picture of SDR Console V3): "I like
// the view of the broadcast fm spectrum where it shows what each frequency
// group is used for and thought it might be nice to add this. I am just not
// sure how you can have this only show up for certain use cases."
//
// THE ANSWER TO THE SECOND HALF IS THE FIRST HALF. This is not a setting
// anybody has to find and it is not a preference: the multiplex exists in WFM
// and nowhere else, so the scope input that shows it exists in WFM and nowhere
// else, and the labels come with it. An AM station has no pilot; an SSB
// channel has no stereo subcarrier; a decoder plugin's audio has no multiplex
// at all. Nothing has to be hidden, because nothing is offered.
//
// WHAT THE MULTIPLEX IS, which is what makes the labels worth drawing. The FM
// broadcast composite - the discriminator's output, before de-emphasis - is
// several signals stacked in frequency rather than one:
//
//     0 .. 15 kHz    the mono sum (L+R), flat, and all a mono set ever hears
//        19 kHz      the stereo pilot, a single low-level tone
//    23 .. 53 kHz    the difference (L-R) on a SUPPRESSED 38 kHz carrier,
//                    which is why it reads as two sidebands with a hole in
//                    the middle
//        57 kHz      RDS/RBDS - the station name and radiotext, on the third
//                    harmonic of the pilot
//     67.65, 92 kHz  SCA: subsidiary services (background music, reading
//                    services for the blind, data), present on some stations
//                    and absent on most
//
// Seeing that stack labelled is how a person learns why a station is or is not
// in stereo, why RDS appears on one and not another, and what the noise above
// 60 kHz on a weak signal actually is. core/stereo_fm.hpp carries the same
// table in prose and is where the DECODER's version of these numbers lives;
// they are repeated here as DRAWING data, and test_fm_mpx_plan.cpp pins the
// two against each other so a change to one cannot silently disagree with the
// other.
//
// WHY A PURE HEADER. Everything here is arithmetic on frequencies and pixel
// widths: which bands fall inside the span, where each lands on the glass, and
// which labels are too cramped to be worth drawing. None of it needs a draw
// list, so all of it is testable without one - the same split as
// gui/scope_view.hpp and gui/volume_meter.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_FM_MPX_PLAN_HPP
#define CASCADE_GUI_FM_MPX_PLAN_HPP

#include <cstddef>

namespace cascade::gui {

// A region of the multiplex worth naming on the glass.
//
// `loHz == hiHz` marks a TONE rather than a band (the pilot): one rule, one
// label, no shaded width. Everything else is drawn as an extent.
struct MpxBand {
    double loHz = 0.0;
    double hiHz = 0.0;
    const char* label = "";  // what it is, in the fewest words that are true
    const char* note = "";   // the frequency, spelled for the eye ("19 kHz")
    // Whether this is one of the things a station may simply not transmit.
    // Drawn fainter: an empty 57 kHz slot is a fact about the station, and a
    // label in the same ink as the pilot's would read as a fault.
    bool optional = false;
};

// THE MULTIPLEX, in the order a reader meets it going up the axis.
//
// The pilot is 19 kHz exactly; the difference sidebands are 38 +/- 15 kHz,
// which is 23..53 with the suppressed carrier in the middle; RDS is 57 kHz
// (three times the pilot) with its own +/-2.4 kHz of sidebands; the two SCA
// slots in common use are 67.65 kHz and 92 kHz. The sum channel stops at
// 15 kHz because that is where the standard's audio bandwidth stops, not
// because of anything in this receiver.
inline constexpr MpxBand kMpxBands[] = {
    {0.0, 15000.0, "Mono L+R", "0-15 kHz", false},
    {19000.0, 19000.0, "Pilot", "19 kHz", false},
    {23000.0, 38000.0, "Stereo L-R", "lower", true},
    {38000.0, 53000.0, "Stereo L-R", "upper", true},
    {54600.0, 59400.0, "RDS", "57 kHz", true},
    {67650.0, 67650.0, "SCA", "67.65 kHz", true},
    {92000.0, 92000.0, "SCA", "92 kHz", true},
};
inline constexpr std::size_t kMpxBandCount = sizeof(kMpxBands) / sizeof(kMpxBands[0]);

// THE SPAN THE MULTIPLEX IS DRAWN OVER: nought to a hundred kilohertz, or the
// composite's own Nyquist when that is lower.
//
// A hundred is where the multiplex stops being anything: the highest thing the
// standard puts there is the 92 kHz SCA, and a broadcaster's own transmitter
// filtering takes the rest away. The Nyquist floor is the same rule the audio
// spectrum follows (gui/demod_scope.hpp) and exists for the same reason - a
// scope must never rule an axis over frequencies the chain cannot carry. The
// channel rate this application targets is 200 kHz, so the two are 100 and 100
// and neither wins; a device forced to a narrower channel gets a shorter axis
// rather than a lie.
inline constexpr double kMpxSpanMaxHz = 100000.0;

inline double mpxSpanHz(double compositeRateHz) {
    if (!(compositeRateHz > 0.0)) { return 0.0; }
    const double nyquist = compositeRateHz * 0.5;
    return (nyquist < kMpxSpanMaxHz) ? nyquist : kMpxSpanMaxHz;
}

// Where a frequency sits across a tube ruled from 0 to spanHz, as a fraction.
// Outside the span answers outside 0..1 rather than clamping, so a caller can
// tell "off the left" from "at the left" - the culling below depends on it.
inline double mpxFraction(double hz, double spanHz) {
    if (!(spanHz > 0.0)) { return 0.0; }
    return hz / spanHz;
}

// Is this band worth drawing at all on a tube of this span? A band entirely
// past the right-hand edge is not: the 92 kHz SCA on a chain that can only
// carry 60 kHz is not "absent from this station", it is outside what this
// receiver is looking at, and a label that did not distinguish those two would
// be worse than no label.
inline bool mpxBandInSpan(const MpxBand& b, double spanHz) {
    if (!(spanHz > 0.0)) { return false; }
    return b.loHz < spanHz || (b.loHz == b.hiHz && b.loHz <= spanHz);
}

// HOW MUCH ROOM A BAND'S LABEL HAS, in pixels, on a tube `widthPx` wide.
//
// A tone gets the distance to its neighbours rather than zero - the pilot is
// one rule and its name still has to go somewhere - and a band gets its own
// width. The caller compares this against the width of the text it is about to
// draw and skips the ones that will not fit, which is the whole of the
// crowding rule: on a narrow scope only "Mono L+R" survives, and that is the
// right answer for a 200-pixel tube.
inline float mpxLabelRoomPx(const MpxBand& b, double spanHz, float widthPx) {
    if (!(spanHz > 0.0) || !(widthPx > 0.0f)) { return 0.0f; }
    const double lo = (b.loHz < spanHz) ? b.loHz : spanHz;
    const double hi = (b.hiHz < spanHz) ? b.hiHz : spanHz;
    if (hi > lo) {
        return static_cast<float>((hi - lo) / spanHz * static_cast<double>(widthPx));
    }
    // A tone: half the gap to each side, bounded by the tube.
    const double kToneRoomHz = 9000.0;  // the pilot to the sum channel's edge
    const double left = (lo - kToneRoomHz > 0.0) ? lo - kToneRoomHz : 0.0;
    const double right = (lo + kToneRoomHz < spanHz) ? lo + kToneRoomHz : spanHz;
    return static_cast<float>((right - left) / spanHz * static_cast<double>(widthPx));
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_FM_MPX_PLAN_HPP
