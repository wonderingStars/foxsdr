// meter_styles_math.hpp - the arithmetic behind the four bench meter faces,
// with no ImGui in it, so tests/test_meter_styles_math.cpp can drive every
// decision without a graphics context - the same split volume_meter.hpp
// keeps between the reading (pure) and the needle (drawn).
//
// WHY FOUR FACES. An Italian user on 0.99.42: "is it possible to customise
// the VU meter, choosing between 3 or 4 different VU meters?" The owner's
// answer is four, picked by right-clicking the meter itself (app_window.cpp,
// AppWindow::drawMeterStyleMenu) - Classic (the face drawn since 0.87.0,
// unchanged and the default), Analogue needle (a moving-coil VU with a
// filled red zone), LED ladder (a segmented bar) and Peak meter (a
// PPM-style bar with a peak-hold tick).
//
// EVERY STYLE READS THE SAME NUMBER. `frac01` still comes from
// gui::meterFraction fed through gui::meterBallistics exactly as it always
// has - this header adds no second opinion about what the needle is doing,
// only new ways to draw the one figure. A meter that read a different value
// depending which face it wore would be worse than any single face: the
// reference artboard's own needle-disagrees-with-the-text mistake, in a
// different place.
//
// PEAK HOLD IS A SEPARATE, PURE STRUCT because the LED ladder and the peak
// meter both need to remember a level a while after it has passed - the same
// reason a real PPM has a mechanical catch on its pointer. It rises
// INSTANTLY (a transient must never be under-read) and falls at a fixed
// rate, on the meterBallistics rule for a stalled or first frame: a
// non-finite or out-of-range `dt` leaves the held peak exactly where it is
// rather than teleporting it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_METER_STYLES_MATH_HPP
#define CASCADE_GUI_METER_STYLES_MATH_HPP

#include <algorithm>
#include <string>

#include "core/i18n.hpp"

namespace cascade::gui {

// The four faces a bench meter can wear. Classic is index 0 and the default
// so an existing install's meters look exactly as they always have.
enum class MeterStyle {
    Classic = 0,  // the arc-and-needle face drawn since 0.87.0 - unchanged
    Needle,       // a moving-coil VU: full arc scale, needle, filled red zone
    LedLadder,    // a segmented LED bar, green/amber/red, with peak hold
    Peak,         // a horizontal PPM-style bar with a peak-hold tick
};

// THE NAME IS WHAT THE CONFIG FILE CARRIES, on the tunerDisplayStyle rule
// (gui/tune_control.hpp): an unknown or empty value is the DEFAULT, never a
// refusal, because the file is user-editable and a typo must leave the
// meter looking like itself. Matching is exact and lower-case.
inline MeterStyle meterStyleFromName(const std::string& name) {
    if (name == "needle") { return MeterStyle::Needle; }
    if (name == "led") { return MeterStyle::LedLadder; }
    if (name == "peak") { return MeterStyle::Peak; }
    return MeterStyle::Classic;
}

inline const char* meterStyleName(MeterStyle style) {
    switch (style) {
        case MeterStyle::Needle: return "needle";
        case MeterStyle::LedLadder: return "led";
        case MeterStyle::Peak: return "peak";
        case MeterStyle::Classic: break;
    }
    return "classic";
}

// The four names in the order the right-click menu offers them, so the
// picker and the config vocabulary cannot drift apart - kTunerStyleNames'
// own rule.
inline constexpr int kMeterStyleCount = 4;
inline const char* kMeterStyleNames[kMeterStyleCount] = {"classic", "needle", "led", "peak"};
// What the picker LABELS them. "Classic" is the same key the band plan
// palette picker already uses (kBandPlanPaletteLabels in app_window.cpp) -
// one translated word, not two copies of it drifting apart in 33
// catalogues.
inline const char* kMeterStyleLabels[kMeterStyleCount] = {
    FOX_TR_NOOP("Classic"), FOX_TR_NOOP("Analogue needle"), FOX_TR_NOOP("LED ladder"),
    FOX_TR_NOOP("Peak meter")};

// How many fraction of full scale the held peak gives back every second once
// nothing newer has beaten it - slow enough to be read, the same job
// volume_meter.hpp's kVolumeFallS does for the needle itself but as a linear
// ramp rather than an exponential: a peak LAMP holds a level, it does not
// glide back down to it.
inline constexpr float kMeterPeakHoldFallPerS = 0.3f;

// The peak-hold catch: rises instantly to a new, higher reading and falls at
// kMeterPeakHoldFallPerS thereafter, never below the CURRENT reading (a held
// peak that read lower than the live needle would be a peak of nothing).
// PURE AND STATEFUL ON PURPOSE - one instance per meter, carried between
// frames by the caller (AppWindow::meterPeakHoldVolume_ /
// meterPeakHoldRate_), exactly the way volumeNeedle_ carries the needle's
// own ballistics.
struct MeterPeakHold {
    float peak = 0.0f;

    // `dt` is the frame time in seconds. A non-finite, zero or too-large dt
    // (the first frame, a stall) leaves the peak exactly where it is rather
    // than teleporting it - meterBallistics' own guard, repeated here because
    // a held peak has exactly the same failure mode a falling needle does.
    void update(float frac01, float dt) {
        if (!(frac01 >= 0.0f)) { frac01 = 0.0f; }  // NaN-safe
        if (frac01 > 1.0f) { frac01 = 1.0f; }
        if (frac01 >= peak) {
            peak = frac01;
            return;
        }
        if (!(dt > 0.0f) || !(dt < 1.0f)) { return; }
        peak -= kMeterPeakHoldFallPerS * dt;
        if (peak < frac01) { peak = frac01; }  // never below the live reading
        if (peak < 0.0f) { peak = 0.0f; }
    }
};

// --- THE LED LADDER ---------------------------------------------------------
//
// Twelve segments, the way a graphic bar-graph VU is actually built: enough
// to read a level at a glance, few enough that each one is a real rectangle
// on a 126x66 px face rather than a smear.
inline constexpr int kMeterLedSegments = 12;

// How many of the twelve are LIT for a 0..1 reading. Rounded rather than
// floored, so a reading sitting exactly on a segment boundary lights that
// segment instead of leaving it dark by one ULP - the same complaint a
// linear needle avoids by rounding its own tick positions.
inline int meterLedLitCount(float frac01) {
    if (!(frac01 > 0.0f)) { return 0; }  // silence, and NaN-safe
    if (frac01 > 1.0f) { frac01 = 1.0f; }
    const int lit = static_cast<int>(frac01 * static_cast<float>(kMeterLedSegments) + 0.5f);
    return std::clamp(lit, 0, kMeterLedSegments);
}

enum class MeterLedZone { Green, Amber, Red };

// The colour zone a given segment (0 = the bottom of the ladder) belongs to.
// The top two segments are the alarm colour - the same "top of the travel
// should be uncomfortable to sit on" rule drawBenchMeter's own tick ladder
// already follows (its last two of nine ticks) - the next three are the
// caution amber, and the rest are the ordinary green run.
inline MeterLedZone meterLedZone(int segmentIndex, int segmentCount) {
    if (segmentIndex >= segmentCount - 2) { return MeterLedZone::Red; }
    if (segmentIndex >= segmentCount - 5) { return MeterLedZone::Amber; }
    return MeterLedZone::Green;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_METER_STYLES_MATH_HPP
