// CW pitch helper: for a pitch of 300 to 1,200 Hz, a sideband and a filter width, the numbers the Weaver chain of dsp::Demodulator needs so
// that a carrier ON the tuned frequency comes out at the chosen pitch and the audio pass band is CENTRED on it.
//
// HOW THE EXISTING CW MODE WORKS (src/dsp/demod.cpp, read before this was written):
//   - the pitch is the constant kCwToneHz = 700.0 (demod.cpp:100);
//   - Demodulator::setMode(CW) (demod.cpp:222-228) sets shiftDown_ to (700 / rate - bfo) and shiftUp_ to +bfo, where bfo is the SSB
//     chain's f0 = (lo + hi) / 2 for the 3 kHz SSB filter (about 1.95 kHz), i.e. "USB with the input pre-shifted up by 700 Hz";
//   - so a carrier on the tuned frequency does beat at 700 Hz (the tests below measure it on the real class), BUT the filter is the voice
//     SSB filter: its pass band in the audio domain is about 300 Hz to 3.6 kHz, centred near 1.95 kHz, not on 700 Hz, and its width
//     follows setSsbBandwidthHz, not a CW width. The pitch is not adjustable and the filter is not a CW filter.
//
// THE CHAIN THESE NUMBERS DRIVE (the same shape as Demodulator::processSsb): mix by firstMixHz, real-tap low-pass with -6 dB point
// lowpassCutoffHz, mix by secondMixHz, take the real part. With the BFO equal to the pitch the first mix is 0 Hz, the low-pass sits on the
// carrier (so it is centred on the tuned frequency), and the second mix moves it to the pitch: audio = pitch + d for the upper sideband and
// pitch - d for the lower, d being the offset of a signal from the tuned frequency. The two sidebands differ only in which way a signal
// above the zero-beat moves the pitch (the usual CW-upper / CW-lower, "CW-R", choice).
//
// Pure maths, no state, no allocation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cascade::dsp {

enum class CwSideband { Upper, Lower };

constexpr double kCwMinPitchHz = 300.0;
constexpr double kCwMaxPitchHz = 1200.0;
constexpr double kCwDefaultPitchHz = 700.0;   // what demod.cpp hard-codes today
constexpr double kCwMinWidthHz = 50.0;
constexpr double kCwDefaultWidthHz = 500.0;

struct CwTuning {
    double pitchHz = kCwDefaultPitchHz;  // after clamping
    double widthHz = kCwDefaultWidthHz;  // -6 dB width of the audio pass band, after clamping
    bool pitchClamped = false;
    bool widthClamped = false;
    // For the Weaver chain, in the sign convention of Demodulator (Nco frequencies in Hz; divide by the channel rate for Nco::setFrequency):
    double bfoHz = kCwDefaultPitchHz;    // f0 (= the pitch: the pass band is centred)
    double firstMixHz = 0.0;             // shiftDown_ : Upper (pre-shift +pitch) - bfo ; Lower: -pitch + bfo
    double secondMixHz = kCwDefaultPitchHz;  // shiftUp_ : Upper +bfo ; Lower -bfo
    double lowpassCutoffHz = 0.5 * kCwDefaultWidthHz;  // real-tap low-pass -6 dB point in the shifted domain
    double lowpassTransitionHz = 0.0;    // suggested total transition width of that low-pass
    // Audio pass band (-6 dB edges) of the result, in Hz of the OUTPUT audio:
    double lowEdgeHz = 0.0;
    double highEdgeHz = 0.0;
};

// Clamps the pitch to [300, 1200] Hz and the width to [50 Hz, 2 (pitch - 50 Hz)] (so the lower edge keeps 50 Hz from DC and the mirror
// image of the pass band does not overlap it), and to 0.4 x the channel rate on the upper side. Non-finite values mean the defaults.
// `channelRateHz` only bounds the width; any value <= 0 is read as 48 kHz.
inline CwTuning cwTuning(double pitchHz, CwSideband sideband, double widthHz = kCwDefaultWidthHz, double channelRateHz = 48000.0) {
    CwTuning t;
    double p = std::isfinite(pitchHz) ? pitchHz : kCwDefaultPitchHz;
    double w = std::isfinite(widthHz) ? widthHz : kCwDefaultWidthHz;
    const double rate = (std::isfinite(channelRateHz) && channelRateHz > 0.0) ? channelRateHz : 48000.0;
    const double pc = std::clamp(p, kCwMinPitchHz, kCwMaxPitchHz);
    t.pitchClamped = pc != p;
    p = pc;
    double wMax = 2.0 * (p - 50.0);
    wMax = std::min(wMax, 2.0 * (0.4 * rate - p));
    wMax = std::max(wMax, kCwMinWidthHz);
    const double wc = std::clamp(w, kCwMinWidthHz, wMax);
    t.widthClamped = wc != w;
    w = wc;
    t.pitchHz = p;
    t.widthHz = w;
    t.bfoHz = p;
    t.firstMixHz = (sideband == CwSideband::Upper) ? (+p - t.bfoHz) : (-p + t.bfoHz);
    t.secondMixHz = (sideband == CwSideband::Upper) ? +t.bfoHz : -t.bfoHz;
    t.lowpassCutoffHz = 0.5 * w;
    t.lowpassTransitionHz = std::clamp(0.5 * w, 50.0, 400.0);
    t.lowEdgeHz = p - 0.5 * w;
    t.highEdgeHz = p + 0.5 * w;
    return t;
}

// Odd FIR length for a Hamming windowed-sinc with the given total transition (transition about 3.3 rate / N), capped at 2047 taps like the
// SSB filter (a capped design reports its wider transition through *actualTransitionHz).
inline std::size_t cwFilterTaps(double channelRateHz, double transitionHz, double* actualTransitionHz = nullptr) {
    const double rate = (std::isfinite(channelRateHz) && channelRateHz > 0.0) ? channelRateHz : 48000.0;
    const double tr = (std::isfinite(transitionHz) && transitionHz > 1.0) ? transitionHz : 50.0;
    std::size_t n = static_cast<std::size_t>(std::ceil(3.3 * rate / tr));
    n = std::clamp<std::size_t>(n, 63, 2047);
    if (n % 2 == 0) { ++n; }
    if (actualTransitionHz != nullptr) { *actualTransitionHz = 3.3 * rate / static_cast<double>(n); }
    return n;
}

}  // namespace cascade::dsp
