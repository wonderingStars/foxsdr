// demod_scope.hpp - the arithmetic of the DEMOD SCOPE: its time base, its
// trigger, its gain ladder, its graticule and the two reductions that turn a
// buffer of samples into a trace.
//
// WHY THIS IS A SEPARATE, ImGui-FREE HEADER. The same reason gui/scope_view.hpp
// is one, and gui/rail_banks.hpp, and gui/tune_control.hpp: what the scope
// COMPUTES can be pinned by a test without a graphics context, and what it
// DRAWS cannot. Everything that needs an ImDrawList lives in
// gui/demod_scope_face.hpp; everything below is a value conversion. It is also
// held by value inside AppWindow, which is compiled into the tests under a
// standing rule that app_window.hpp pulls in neither GLFW nor ImGui.
//
// WHAT AN OSCILLOSCOPE IS, as this file understands it. A tube ten divisions
// wide and eight high; a sweep that starts at a trigger and runs left to
// right; a vertical scale in units per division; and a trace drawn as the
// envelope of whatever fell in each column, not as a polyline through
// hand-picked samples. All four of those are numbers, and all four are here.
//
// THE ONE THING THAT IS NOT OBVIOUS IS THE TRIGGER, so it is worth saying
// plainly. A bare rising zero crossing does NOT hold speech still: a vowel
// crosses zero several times per pitch period, and the sweep lands on a
// different one of those crossings each frame, so the picture slides. HOLD-OFF
// is what fixes it, and here it means one specific thing - the signal must
// have stayed continuously BELOW the lower hysteresis threshold for at least
// `holdoff` samples before a rising crossing counts. That rejects the small
// ripples inside a period and accepts only the one crossing that follows the
// quiet part of the cycle, which is what makes a vowel stand still.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DEMOD_SCOPE_HPP
#define CASCADE_GUI_DEMOD_SCOPE_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace cascade::gui {

// --- the tube ----------------------------------------------------------------
//
// Ten by eight, the divisions every bench scope from the period was ruled in.
// Both are EVEN, which is what gives the graticule a centre line on each axis -
// the zero volts line and the mid-sweep line - and the drawing leans on that.
inline constexpr int kScopeDivX = 10;
inline constexpr int kScopeDivY = 8;

// The i'th graticule line across a span, 0 at x0 and `divisions` at x1.
// Out-of-range indices are clamped rather than extrapolated: a caller that
// walks one too far draws over a line it already drew instead of over the
// bezel beside it.
inline float scopeGridLine(int i, int divisions, float a, float b) {
    if (divisions <= 0) { return a; }
    int n = i;
    if (n < 0) { n = 0; }
    if (n > divisions) { n = divisions; }
    return a + (b - a) * (static_cast<float>(n) / static_cast<float>(divisions));
}

// Whether a graticule line is the axis rather than an ordinary rule. Only an
// even division count has one, and both of this tube's do.
inline bool scopeGridIsCentre(int i, int divisions) {
    return divisions > 0 && (divisions % 2) == 0 && i == divisions / 2;
}

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
    // THE FIFTH POSITION EXISTS ONLY IN WFM - see scopeSignalAvailable below.
    // The broadcast multiplex is the discriminator's output before de-emphasis
    // and before the stereo decoder: the mono sum, the pilot, the difference
    // sidebands, RDS and any SCA, stacked in frequency. Nothing else this
    // receiver demodulates has one, which is why this is not a setting anybody
    // has to find - in every other mode the key is simply not there.
    Mpx = 4
};
inline constexpr int kScopeSignalCount = 5;

// The word engraved on the key. Short, because four keys share one row.
inline const char* scopeSignalKey(ScopeSignal s) {
    switch (s) {
        case ScopeSignal::Audio: return "AUDIO";
        case ScopeSignal::Spectrum: return "SPEC";
        case ScopeSignal::Baseband: return "I/Q";
        case ScopeSignal::Vector: return "VECTOR";
        case ScopeSignal::Mpx: return "MPX";
    }
    return "";
}

// What the readout under the tube calls it - the long form, because that line
// is where somebody finds out what they are looking at.
inline const char* scopeSignalCaption(ScopeSignal s) {
    switch (s) {
        case ScopeSignal::Audio: return "DEMODULATED AUDIO";
        case ScopeSignal::Spectrum: return "AUDIO SPECTRUM";
        case ScopeSignal::Baseband: return "BASEBAND I/Q";
        case ScopeSignal::Vector: return "VECTOR I-Q";
        case ScopeSignal::Mpx: return "FM MULTIPLEX";
    }
    return "";
}

// A saved selector position, clamped. The config carries an int and a hand
// edit can say anything; whatever it says, the scope opens on a signal that
// exists.
inline ScopeSignal scopeSignalFromIndex(int index) {
    if (index < 0) { return ScopeSignal::Audio; }
    if (index >= kScopeSignalCount) { return ScopeSignal::Vector; }
    return static_cast<ScopeSignal>(index);
}

// WHICH POSITIONS EXIST RIGHT NOW. Four always; the multiplex only while the
// receiver is demodulating wideband FM, because only then is there one.
//
// Asked as a question rather than written out at the call sites so the key
// row, the caption, the saved-position fallback and the page's feed all agree
// by construction - and so the day a second mode grows a multiplex-like
// signal, this is the one line that changes.
inline bool scopeSignalAvailable(ScopeSignal s, bool wfm) {
    return (s != ScopeSignal::Mpx) || wfm;
}

// THE SAVED POSITION, AGAINST TODAY'S MODE. A user who left the scope on MPX
// and came back on AM must not be shown an empty tube with a caption naming a
// signal that does not exist; they get the audio spectrum, which is the
// nearest thing that does. The saved setting is NOT rewritten by this - go
// back to WFM and the multiplex is there again, exactly as it was left.
inline ScopeSignal scopeSignalForMode(ScopeSignal saved, bool wfm) {
    return scopeSignalAvailable(saved, wfm) ? saved : ScopeSignal::Spectrum;
}

// The two I/Q signals are the two that need the baseband tap rather than the
// audio one. Asked as a question so no call site has to remember the list.
inline bool scopeSignalIsBaseband(ScopeSignal s) {
    return s == ScopeSignal::Baseband || s == ScopeSignal::Vector;
}

// --- is anything arriving? ----------------------------------------------------
//
// A TAP IS LIVE IF ITS COUNTER HAS MOVED RECENTLY, and "recently" is measured
// on the CLOCK, never frame to frame. The first version of this asked "did the
// counter change since the last frame", which is the single-instant metric
// this project has been bitten by before: at the ~1400 frames a second this
// application actually renders at, a frame is shorter than one DSP block, so
// on most frames nothing HAS arrived - and the scope lettered "NO SAMPLES -
// RECEIVER STOPPED" across a running receiver. The quantity that matters is
// whether samples are arriving at all, and that is a question about a window
// of time.
//
// A quarter of a second is comfortably longer than any block period the chain
// produces (a 2 MS/s source publishes audio every half millisecond, and even
// an 8 kHz one manages tens of milliseconds) and short enough that stopping
// the receiver puts the words on the glass while the hand is still on the key.
inline constexpr double kScopeStaleSeconds = 0.25;

// The caller's memory of one tap. One per tap, because the page reads a
// different one depending on which input is selected and a shared record would
// answer for whichever was looked at last.
struct ScopeLiveness {
    std::uint64_t lastSeen = 0;
    double lastChangeS = 0.0;
    bool started = false;
};

// `written` is the tap's monotonic counter, `nowS` any monotonic clock in
// seconds. Returns whether the tap is producing.
//
// A COUNTER OF ZERO IS NEVER LIVE, whatever the clock says: nothing has ever
// been written, which is not the same statement as "it is quiet" and must not
// be allowed to read as one.
inline bool scopeTapLive(ScopeLiveness& st, std::uint64_t written, double nowS) {
    if (!st.started) {
        st.started = true;
        st.lastSeen = written;
        st.lastChangeS = nowS;
        return written > 0;
    }
    if (written != st.lastSeen) {
        st.lastSeen = written;
        st.lastChangeS = nowS;
    }
    return written > 0 && (nowS - st.lastChangeS) < kScopeStaleSeconds;
}

// --- the settings the panel keeps ---------------------------------------------
//
// EVERYTHING THE KEYS OPERATE AND THE CONFIG PERSISTS, in one value. Here
// rather than beside the drawing because AppWindow holds one by value and
// app_window.hpp may not reach imgui.h; and as plain ints rather than as the
// enums they index, because that is what config.json carries and a load of a
// hand-edited file has to be able to hold whatever it found until the clamps
// above have had a look at it.
struct DemodScopeState {
    int signal = 0;     // a ScopeSignal; AUDIO, which is what the page is for
    int timebase = 3;   // an index into kScopeTimebaseMs; 3 is 10 ms/DIV
    int gainIndex = 5;  // an index into kScopeGainPerDiv; 5 is 100 mV/DIV
    // AUTO ON BY DEFAULT, because the alternative is a first sight of the
    // scope showing a flat line at a gain nobody chose - and a user who has
    // never operated an attenuator would read that as the feature not working.
    bool autoGain = true;

    bool operator==(const DemodScopeState&) const = default;
};

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

inline double scopeTimebaseMs(int index) {
    return kScopeTimebaseMs[clampScopeTimebase(index)];
}

// Samples across the WHOLE tube at this time base and rate - ten divisions,
// not one. Zero for a rate that is not a positive finite number, which is what
// an unopened device and a stopped pipeline both report, and drawing nothing
// is the honest answer to both.
inline std::size_t scopeSweepSamples(double msPerDiv, double rateHz) {
    if (!(msPerDiv > 0.0) || !(rateHz > 0.0)) { return 0; }
    const double n = msPerDiv * 1.0e-3 * rateHz * static_cast<double>(kScopeDivX);
    if (!(n >= 1.0)) { return 0; }
    // A tube is about a thousand pixels wide; a sweep longer than this cannot
    // be seen and is only a bigger memcpy. It is also the guard that stops a
    // 61 MS/s baseband rate asking for a hundred million samples.
    constexpr double kMaxSweep = 4.0 * 1024.0 * 1024.0;
    if (n > kMaxSweep) { return static_cast<std::size_t>(kMaxSweep); }
    return static_cast<std::size_t>(n);
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

inline float scopeGainPerDiv(int index) { return kScopeGainPerDiv[clampScopeGain(index)]; }

// The value at the top of the tube for a given step: four divisions up.
inline float scopeFullScale(int index) {
    return scopeGainPerDiv(index) * static_cast<float>(kScopeDivY / 2);
}

// AUTO AMPLITUDE, one step at a time.
//
// Returns the attenuator step this peak asks for, given where the attenuator
// is now. ONE STEP PER CALL, which at frame rate is a smooth ranging rather
// than a jump, and is also what stops a transient throwing the scale three
// steps away from where the rest of the signal lives.
//
// THE TWO THRESHOLDS ARE DELIBERATELY FAR APART (0.95 and 0.30 of full scale)
// and that gap is the whole design. A single threshold makes the attenuator
// hunt: the step that brings a signal just under full scale puts it just over
// the "too small" line of the step below, and it oscillates between the two
// for ever. With this gap, a signal that has settled anywhere between 30% and
// 95% of the tube is left alone.
//
// A PEAK OF ZERO HOLDS THE CURRENT STEP. Silence is not a measurement of
// amplitude, and winding the attenuator to its most sensitive step during a
// pause would make the noise floor leap up the tube the instant the signal
// stopped - the display equivalent of the clean-zero fault this product has
// been bitten by before.
inline int scopeAutoGain(float peak, int currentIndex) {
    const int cur = clampScopeGain(currentIndex);
    if (!(peak > 0.0f)) { return cur; }
    const float full = scopeFullScale(cur);
    if (peak > full * 0.95f) { return clampScopeGain(cur + 1); }
    if (peak < full * 0.30f) { return clampScopeGain(cur - 1); }
    return cur;
}

// The largest magnitude in a buffer, which is what scopeAutoGain is fed. NaN
// samples are ignored rather than propagated: one of them would otherwise make
// every comparison in the ranging false and freeze the attenuator for good.
inline float scopePeak(const float* x, std::size_t n) {
    float peak = 0.0f;
    if (x == nullptr) { return peak; }
    for (std::size_t i = 0; i < n; ++i) {
        const float a = std::fabs(x[i]);
        if (a > peak) { peak = a; }
    }
    return peak;
}

// --- the trigger -------------------------------------------------------------

// What scopeFindTrigger returns when there is no usable edge. The largest
// size_t there is, so it can never be mistaken for an index into any buffer
// that actually exists.
inline constexpr std::size_t kScopeNoTrigger = static_cast<std::size_t>(-1);

// A RISING CROSSING OF `level`, WITH HYSTERESIS AND HOLD-OFF.
//
// Scans forward through x[0..n) and returns the index of the first crossing
// that (a) is preceded by at least `holdoff` consecutive samples strictly
// below level - hysteresis, and (b) leaves `need` samples after it, so the
// whole sweep can be drawn from that point. kScopeNoTrigger when there is
// none, which is the free-running case and the caller's cue to sweep from the
// start of the buffer rather than to draw nothing.
//
// See the header note for why hold-off is defined as "time spent below the
// lower threshold" rather than as "time since the last trigger": this function
// finds ONE trigger in ONE buffer, so a rule about the previous trigger would
// have nothing to measure from.
inline std::size_t scopeFindTrigger(const float* x, std::size_t n, std::size_t need,
                                    float level, float hysteresis,
                                    std::size_t holdoff) {
    if (x == nullptr || n == 0 || need > n) { return kScopeNoTrigger; }
    const float lower = level - hysteresis;
    const std::size_t last = n - need;  // the last index that leaves room
    // `armed` is "has been below the lower threshold since it was last above
    // the level", and belowRun is how long that stretch has run. The two are
    // separate so a hold-off of zero still means hysteresis - the signal must
    // have genuinely dipped below `lower`, not merely sat in the dead band.
    bool armed = false;
    std::size_t belowRun = 0;
    if (x[0] < lower) {
        armed = true;
        belowRun = 1;
    }
    bool prevAbove = !(x[0] < level);
    for (std::size_t i = 1; i <= last; ++i) {
        const float v = x[i];
        const bool above = !(v < level);
        if (above) {
            // A rising crossing: the sample before it was under the trigger
            // level and this one is not.
            if (!prevAbove && armed && belowRun >= holdoff) { return i; }
            armed = false;
            belowRun = 0;
        } else if (v < lower) {
            ++belowRun;
            armed = true;
        }
        // Between the two thresholds, neither arms nor disarms, and - the
        // point of the dead band - does not reset the run either. A signal on
        // its way up through the hysteresis has not stopped being the same
        // stretch of quiet that armed the trigger.
        prevAbove = above;
    }
    return kScopeNoTrigger;
}

// HOW LONG THE SIGNAL MUST HAVE BEEN QUIET, as a fraction of the sweep rather
// than as a number of samples or of milliseconds.
//
// A QUARTER OF ONE DIVISION, and the choice is the whole of the setting. Too
// short and it stops rejecting the ripples inside a period - the trace slides.
// Too long and the trigger can only fire on signals slower than the hold-off
// itself, and a tone that ought to stand still never triggers at all. A
// quarter division scales with the time base, which is what makes one rule
// work from a 1 kHz tone at 1 ms/DIV to a syllable at 50.
inline std::size_t scopeHoldoffSamples(std::size_t sweepSamples) {
    return sweepSamples / (static_cast<std::size_t>(kScopeDivX) * 4u);
}

// The trigger's dead band, as a fraction of what fills the tube. Proportional
// rather than absolute because the attenuator moves: a hysteresis of a fixed
// 20 mV is half the screen at the most sensitive step and invisible at the
// coarsest, and a trigger that behaves differently at different gains is one
// nobody can predict.
inline float scopeHysteresis(int gainIndex) { return scopeFullScale(gainIndex) * 0.02f; }

// --- the trace ---------------------------------------------------------------

// REDUCE A SWEEP TO ONE MIN/MAX PAIR PER COLUMN, which is how a scope draws
// and is not the same thing as sampling every n'th point. A 24 kHz tone on a
// 50 ms/div sweep puts about fifty cycles in each column: taking one sample
// per column would draw an alias - a slow wandering line that is not in the
// signal at all - while the envelope of what fell in the column is the bright
// band a real tube shows.
//
// Returns false (and touches nothing) for an empty input or no columns. When
// there are FEWER samples than columns each column takes the nearest sample,
// so a short buffer draws a coarse line rather than gaps.
inline bool scopeReduce(const float* x, std::size_t n, std::size_t cols, float* lo,
                        float* hi) {
    if (x == nullptr || lo == nullptr || hi == nullptr || n == 0 || cols == 0) {
        return false;
    }
    for (std::size_t c = 0; c < cols; ++c) {
        std::size_t a = (n * c) / cols;
        std::size_t b = (n * (c + 1)) / cols;
        if (a >= n) { a = n - 1; }
        if (b <= a) { b = a + 1; }
        if (b > n) { b = n; }
        float mn = x[a];
        float mx = x[a];
        for (std::size_t i = a + 1; i < b; ++i) {
            if (x[i] < mn) { mn = x[i]; }
            if (x[i] > mx) { mx = x[i]; }
        }
        lo[c] = mn;
        hi[c] = mx;
    }
    return true;
}

// A sample's height on the tube. `divPx` is one division in pixels and
// `yCentre` the zero line; y grows downward, as every screen coordinate in
// this application does.
//
// CLAMPED TO THE GRATICULE, because a tube clips - the beam simply does not go
// past the edge of the phosphor. An unclamped trace would be drawn over the
// bezel, the readouts and whatever page is behind, which is the one thing a
// scope face must never do.
inline float scopeTraceY(float v, float unitsPerDiv, float yCentre, float divPx) {
    const float half = divPx * static_cast<float>(kScopeDivY / 2);
    if (!(unitsPerDiv > 0.0f)) { return yCentre; }
    // NaN-safe by construction: the comparisons below are false for NaN, so a
    // NaN sample lands on the centre line rather than at an undefined pixel.
    const float d = v / unitsPerDiv * divPx;
    if (d > half) { return yCentre - half; }
    if (d < -half) { return yCentre + half; }
    if (!(d == d)) { return yCentre; }
    return yCentre - d;
}

// --- the spectrum mode -------------------------------------------------------

// THE SPAN THE AUDIO SPECTRUM SHOWS: nought to twenty kilohertz, or the sink's
// Nyquist when that is lower. Twenty is where hearing stops; the Nyquist floor
// is there because a scope must never rule an axis over frequencies the chain
// cannot carry - at the 48 kHz this application resamples to the two are 20
// and 24 and the first wins, but a device opened at 16 kHz would otherwise get
// an axis two thirds of which is a lie.
inline constexpr double kScopeSpectrumMaxHz = 20000.0;

inline double scopeSpectrumSpanHz(double rateHz) {
    if (!(rateHz > 0.0)) { return 0.0; }
    const double nyquist = rateHz * 0.5;
    return (nyquist < kScopeSpectrumMaxHz) ? nyquist : kScopeSpectrumMaxHz;
}

// The dB window the log magnitude is drawn in. Eighty decibels over eight
// divisions is ten a division, which is the ruling every spectrum analyser of
// the period used.
inline constexpr float kScopeSpectrumTopDb = 0.0f;
inline constexpr float kScopeSpectrumBottomDb = -80.0f;

// A magnitude to decibels relative to full scale, floored well under the
// window so a zero bin lands on the bottom rule instead of at negative
// infinity.
inline float scopeSpectrumDb(float magnitude) {
    if (!(magnitude > 1.0e-9f)) { return -200.0f; }
    return 20.0f * std::log10(magnitude);
}

// Where a decibel figure sits between y0 (the top rule) and y1 (the bottom).
// Clamped for the reason scopeTraceY is.
inline float scopeSpectrumY(float db, float y0, float y1) {
    const float span = kScopeSpectrumTopDb - kScopeSpectrumBottomDb;
    float t = (kScopeSpectrumTopDb - db) / span;
    if (!(t > 0.0f)) { t = 0.0f; }
    if (t > 1.0f) { t = 1.0f; }
    return y0 + (y1 - y0) * t;
}

// How many of an FFT's bins fall inside the drawn span. The transform produces
// `fftSize` bins of which the first half are the positive frequencies; this is
// the count of those that are at or below `spanHz`.
inline std::size_t scopeSpectrumBins(std::size_t fftSize, double rateHz, double spanHz) {
    if (fftSize == 0 || !(rateHz > 0.0) || !(spanHz > 0.0)) { return 0; }
    const double binHz = rateHz / static_cast<double>(fftSize);
    std::size_t bins = static_cast<std::size_t>(spanHz / binHz) + 1;
    const std::size_t half = fftSize / 2;
    if (bins > half) { bins = half; }
    return bins;
}

// --- the vector mode ---------------------------------------------------------

// One Lissajous point: I across, Q up, both on the same attenuator so a circle
// is drawn as a circle. Clamped to the tube like every other beam position.
inline void scopeVectorPoint(float i, float q, float unitsPerDiv, float cx, float cy,
                             float divPx, float& outX, float& outY) {
    // The tube is wider than it is tall, so the SHORTER axis sets the scale -
    // otherwise an FM carrier's circle would be drawn as an ellipse and the
    // one thing the vector display is for (the shape) would be wrong.
    const float half = divPx * static_cast<float>(kScopeDivY / 2);
    float dx = 0.0f;
    float dy = 0.0f;
    if (unitsPerDiv > 0.0f) {
        dx = i / unitsPerDiv * divPx;
        dy = q / unitsPerDiv * divPx;
    }
    if (!(dx == dx)) { dx = 0.0f; }
    if (!(dy == dy)) { dy = 0.0f; }
    if (dx > half) { dx = half; }
    if (dx < -half) { dx = -half; }
    if (dy > half) { dy = half; }
    if (dy < -half) { dy = -half; }
    outX = cx + dx;
    outY = cy - dy;
}

// --- readout formatting ------------------------------------------------------

// The time base as a bench instrument letters it: "5 ms/DIV", and "500 us/DIV"
// for anything under a millisecond (nothing on the current ladder is, but the
// ladder is a constant somebody will extend). Written into `out`, which must
// hold at least 16 characters.
//
// INLINE, WITH NO .cpp BEHIND IT, so that a test of the arithmetic needs
// nothing linked but itself - the same property that makes this whole header
// worth keeping ImGui-free.
inline void formatScopeTimebase(char* out, std::size_t cap, double msPerDiv) {
    if (out == nullptr || cap == 0) { return; }
    if (msPerDiv >= 1.0) {
        std::snprintf(out, cap, "%g ms/DIV", msPerDiv);
    } else {
        std::snprintf(out, cap, "%g us/DIV", msPerDiv * 1000.0);
    }
}

// The attenuator the same way: "20 mV/DIV" in the thousandths that a sample of
// 1.0 being "one volt" makes these, or "0.5 V/DIV" at the coarse end.
inline void formatScopeGain(char* out, std::size_t cap, float unitsPerDiv) {
    if (out == nullptr || cap == 0) { return; }
    if (unitsPerDiv >= 1.0f) {
        std::snprintf(out, cap, "%g V/DIV", static_cast<double>(unitsPerDiv));
    } else {
        std::snprintf(out, cap, "%g mV/DIV",
                      static_cast<double>(unitsPerDiv) * 1000.0);
    }
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DEMOD_SCOPE_HPP
