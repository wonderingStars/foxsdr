// view_settings.hpp - the ladders and ranges of the settings the window keeps
// in config.json, which the CONFIG (engine side) must clamp on load.
//
// WHY THESE LIVE HERE. Each of them indexes something the window draws - the
// demod scope's selector, display keys, time base and attenuator, the radar
// scope's range ladder, the rail's five banks - and config.cpp snaps every one
// of them onto its ladder when a file is read, because an unclamped hand-edit
// would not be a wrong setting but a read off the end of a table. Until the
// engine was split from the window (engine extraction, step 1) they sat in
// gui/demod_scope.hpp, gui/scope_view.hpp and gui/rail_banks.hpp and the
// config reached into gui/ for them. They moved here VERBATIM - same values,
// same clamps, same comments - so the engine can keep applying exactly the
// rule the window draws by without including a window header; the gui headers
// bring every name back into cascade::gui with a using-declaration, so every
// caller there reads exactly as before.
//
// PURE: no ImGui, no GL, no i18n - numbers and the functions that clamp them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_VIEW_SETTINGS_HPP
#define CASCADE_CORE_VIEW_SETTINGS_HPP

#include <cstdlib>

namespace cascade::core {

// =============================================================================
// The DEMOD SCOPE (from gui/demod_scope.hpp)
// =============================================================================

// --- what the scope is looking at --------------------------------------------
//
// ONE INSTRUMENT WITH AN INPUT SELECTOR, not four pages. A bench scope has one
// tube and a switch that says what is on it, and these four are that switch:
// the demodulated audio as a trace, the same audio as a spectrum, the channel
// I/Q as two traces, and the same I/Q as a Lissajous. Keeping them on one
// cabinet is also what makes the comparison possible - AM, FM and SSB look
// different in VECTOR and nearly identical in AUDIO, and that is the lesson.
enum class ScopeSignal : int {
    Audio = 0,       // the demodulated audio, triggered, as a trace
    Spectrum = 1,    // the same audio, as a log-magnitude spectrum
    Baseband = 2,    // the channel I/Q at the demodulator's input, I and Q
    Vector = 3,      // the same I/Q, plotted I against Q
    // THE FIFTH POSITION EXISTS ONLY IN WFM - see scopeSignalAvailable in
    // gui/demod_scope.hpp. The broadcast multiplex is the discriminator's
    // output before de-emphasis and before the stereo decoder: the mono sum,
    // the pilot, the difference sidebands, RDS and any SCA, stacked in
    // frequency. Nothing else this receiver demodulates has one, which is why
    // this is not a setting anybody has to find - in every other mode the key
    // is simply not there.
    Mpx = 4
};
inline constexpr int kScopeSignalCount = 5;

// A saved selector position, clamped. The config carries an int and a hand
// edit can say anything; whatever it says, the scope opens on a signal that
// exists.
inline ScopeSignal scopeSignalFromIndex(int index) {
    if (index < 0) { return ScopeSignal::Audio; }
    if (index >= kScopeSignalCount) { return ScopeSignal::Vector; }
    return static_cast<ScopeSignal>(index);
}

// --- the display mode ------------------------------------------------------
//
// Asked for by a tester looking at the FM multiplex (2026-09-23): "add
// options to the display to either incorporate some averaging or have some
// persistence in the display". Three latched keys, the way a bench scope's
// acquisition and display buttons work:
//   NORM     - the live trace, exactly as before;
//   AVG      - successive traces blended (see gui/scope_memory.hpp): a
//              spectrum settles into its long-term shape, and a triggered
//              waveform sheds its noise;
//   PERSIST  - the live trace over a fading memory of where the beam has
//              been, like a long-persistence phosphor.
enum class ScopeDisplay : int { Normal = 0, Average = 1, Persist = 2 };
inline constexpr int kScopeDisplayCount = 3;

inline int clampScopeDisplay(int index) {
    if (index < 0 || index >= kScopeDisplayCount) { return 0; }
    return index;
}

inline ScopeDisplay scopeDisplayFromIndex(int index) {
    return static_cast<ScopeDisplay>(clampScopeDisplay(index));
}

// --- the time base -----------------------------------------------------------
//
// A 1-2-5 ladder, the sequence every bench instrument's attenuator and time
// base is stepped in, from one millisecond a division (a single cycle of a
// 1 kHz tone spans one division) to fifty (half a second across the tube,
// which is long enough to watch a syllable).
inline constexpr double kScopeTimebaseMs[] = {1.0, 2.0, 5.0, 10.0, 20.0, 50.0};
inline constexpr int kScopeTimebaseCount = 6;

inline int clampScopeTimebase(int index) {
    if (index < 0) { return 0; }
    if (index >= kScopeTimebaseCount) { return kScopeTimebaseCount - 1; }
    return index;
}

// --- the vertical attenuator -------------------------------------------------
//
// Units per DIVISION, on the same 1-2-5 ladder, where one unit is full scale
// of the audio path (a sample of 1.0). Four divisions is the top of the tube,
// so the coarsest step here shows a signal at digital full scale filling half
// the height and the finest resolves about a thousandth of full scale.
inline constexpr float kScopeGainPerDiv[] = {0.002f, 0.005f, 0.01f, 0.02f, 0.05f,
                                             0.1f,   0.2f,   0.5f,  1.0f};
inline constexpr int kScopeGainCount = 9;

inline int clampScopeGain(int index) {
    if (index < 0) { return 0; }
    if (index >= kScopeGainCount) { return kScopeGainCount - 1; }
    return index;
}

// =============================================================================
// The RADAR SCOPE's range ladder (from gui/scope_view.hpp)
// =============================================================================

// THE RANGE STEPS, in nautical miles. Discrete rather than continuous because
// a scope's range is a SETTING an operator states and returns to ("I am on the
// hundred-mile scale"), not a zoom they scrub - and because every ring, every
// label and the corner readout are derived from it, so a free-running value
// would print ranges like "173 NM" on rings nobody chose.
//
// The ladder itself is the one every ADS-B receiver of this shape offers,
// roughly doubling from a circuit-sized 10 NM out to 400, which is past the
// horizon for a ground station at any sane antenna height and therefore past
// anything this radio can hear - and then two more, 800 and 1600, which are
// not for hearing further but for PLACING. At those two the picture is a
// region and a continent, which is what a view that has been dragged off the
// aerial, or an operator asking where the traffic sits in the world, needs; a
// 400 NM ceiling answered "zoom right out" with a greyed key. They are only
// possible because the ground under the face is drawn in the scope's own
// projection (see scopeGroundPoint): a Mercator ground matched at the middle
// is 9% out at the edge of a 400 NM picture and would be out by three
// quarters at 1600.
inline constexpr int kScopeRangesNm[] = {10, 25, 50, 100, 200, 400, 800, 1600};
inline constexpr int kScopeRangeCount =
    static_cast<int>(sizeof(kScopeRangesNm) / sizeof(kScopeRangesNm[0]));

// The index on the ladder of the value closest to `nm`. Total by construction:
// every integer has a nearest entry, so there is no "not on the ladder" answer
// for a caller to forget to handle.
//
// TIES GO TO THE SMALLER RANGE. 150 NM is exactly between 100 and 200, and the
// tighter of the two is the safer reading of an ambiguous instruction: a scope
// set shorter than asked still draws everything inside it correctly and says
// so in its own corner, where one set longer quietly claims reach the user did
// not ask for.
inline int scopeRangeIndex(int nm) {
    int best = 0;
    // long long throughout: `nm` arrives from a hand-edited config and may be
    // INT_MIN, where `nm - 10` in int arithmetic is undefined behaviour rather
    // than a large number.
    long long bestDist = std::llabs(static_cast<long long>(nm) -
                                    static_cast<long long>(kScopeRangesNm[0]));
    for (int i = 1; i < kScopeRangeCount; ++i) {
        const long long d =
            std::llabs(static_cast<long long>(nm) - static_cast<long long>(kScopeRangesNm[i]));
        if (d < bestDist) {  // strict, so a tie keeps the earlier - smaller - entry
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// The ladder value at `index`, bounds-safe: an index outside the ladder is
// clamped rather than read past the end.
inline int scopeRangeNmAt(int index) {
    if (index < 0) { index = 0; }
    if (index >= kScopeRangeCount) { index = kScopeRangeCount - 1; }
    return kScopeRangesNm[index];
}

// The nearest legal range to `nm`. This is what the config sanitizer applies on
// load, and it is not tidiness: the renderer derives four ring radii, four ring
// labels and the corner readout from this number, so a hand-edited 173 would
// reach the drawing code as a scale with no rings anybody chose and a readout
// nobody could reproduce from the ladder. Snapping to the nearest keeps what
// the edit was reaching for; discarding it back to the default would throw the
// user's intent away for the sake of a number that was almost right.
inline int clampScopeRangeNm(int nm) { return scopeRangeNmAt(scopeRangeIndex(nm)); }

// =============================================================================
// The RAIL's banks (from gui/rail_banks.hpp)
// =============================================================================

// The five banks, in the order the rail's captions always had them: what the
// samples pass through, what is made of them, how it is shown, the ways in
// from outside, and the application talking about itself.
enum class RailBank : int { SignalPath = 0, Decode = 1, View = 2, Extend = 3, System = 4 };
inline constexpr int kRailBankCount = 5;

// A saved bank index, clamped. The config carries an int and an old or edited
// file can say anything; whatever it says, the rail opens on a bank that
// exists.
inline RailBank railBankFromIndex(int index) {
    if (index < 0) { return RailBank::SignalPath; }
    if (index >= kRailBankCount) { return RailBank::System; }
    return static_cast<RailBank>(index);
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_VIEW_SETTINGS_HPP
