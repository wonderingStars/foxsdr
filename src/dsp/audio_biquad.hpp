// Second-order sections for the audio-rate blocks of batch 4 (equaliser, high-pass / low-pass, CW peak filter): the
// coefficient formulas of the "Audio EQ Cookbook" and a transposed direct form II runner with double state.
//
// SOURCE: Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients", published as the W3C Working
// Group Note "Audio EQ Cookbook" (editor Raymond Toy), 8 June 2021, https://www.w3.org/TR/audio-eq-cookbook/ (read 2026-11-11):
// peakingEQ, lowShelf, highShelf, LPF, HPF, notch, with A = 10^(dB/40), w0 = 2 pi f0 / fs, alpha = sin(w0) / (2 Q).
// Nothing here is wired into the engine. Header-only, allocation-free.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

namespace cascade::dsp::abq {

constexpr double kPi = 3.14159265358979323846;

struct Coef {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // normalised: a0 = 1 (the default is the identity)
};
struct State {
    double z1 = 0, z2 = 0;
};

inline double clampFreq(double fs, double f) {
    if (!(f > 0.0)) { f = 1e-4 * fs; }  // NaN and non-positive land on the low clamp
    return std::min(std::max(f, 1e-4 * fs), 0.49 * fs);
}
inline double clampQ(double q) {
    if (!(q > 0.01)) { return 0.01; }
    return std::min(q, 1000.0);
}

inline Coef normalise(double b0, double b1, double b2, double a0, double a1, double a2) {
    Coef c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

inline Coef peak(double fs, double f0, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * clampFreq(fs, f0) / fs;
    const double al = std::sin(w) / (2.0 * clampQ(q));
    const double c = std::cos(w);
    return normalise(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
inline Coef lowShelf(double fs, double f0, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * clampFreq(fs, f0) / fs;
    const double al = std::sin(w) / (2.0 * clampQ(q));
    const double c = std::cos(w);
    const double s = 2.0 * std::sqrt(A) * al;
    return normalise(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                     (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
}
inline Coef highShelf(double fs, double f0, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * clampFreq(fs, f0) / fs;
    const double al = std::sin(w) / (2.0 * clampQ(q));
    const double c = std::cos(w);
    const double s = 2.0 * std::sqrt(A) * al;
    return normalise(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                     (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
}
inline Coef lowpass(double fs, double f0, double q) {
    const double w = 2.0 * kPi * clampFreq(fs, f0) / fs;
    const double al = std::sin(w) / (2.0 * clampQ(q));
    const double c = std::cos(w);
    return normalise((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
inline Coef highpass(double fs, double f0, double q) {
    const double w = 2.0 * kPi * clampFreq(fs, f0) / fs;
    const double al = std::sin(w) / (2.0 * clampQ(q));
    const double c = std::cos(w);
    return normalise((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

inline Coef notch(double fs, double f0, double q) {
    const double w = 2.0 * kPi * clampFreq(fs, f0) / fs;
    const double al = std::sin(w) / (2.0 * clampQ(q));
    const double c = std::cos(w);
    return normalise(1.0, -2 * c, 1.0, 1 + al, -2 * c, 1 - al);
}

inline double run(const Coef& k, State& s, double x) {
    const double y = k.b0 * x + s.z1;
    s.z1 = k.b1 * x - k.a1 * y + s.z2;
    s.z2 = k.b2 * x - k.a2 * y;
    return y;
}

// |H(e^jw)| in dB at frequency f (used by the tests).
inline double magnitudeDb(const Coef& k, double fs, double f) {
    const double w = 2.0 * kPi * f / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    const std::complex<double> num = k.b0 + k.b1 * z1 + k.b2 * z2;
    const std::complex<double> den = 1.0 + k.a1 * z1 + k.a2 * z2;
    return 20.0 * std::log10(std::max(std::abs(num / den), 1e-30));
}

}  // namespace cascade::dsp::abq
