// The modulators. See modulator.hpp for what each mode produces and why.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/modulator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cascade::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Where the pre-emphasis is normalised. See Modulator::rebuildPreemphasis.
constexpr double kPreemphasisRefHz = 1000.0;

struct ModeRow {
    TxMode mode;
    const char* name;
    const char* caption;
};

constexpr ModeRow kModes[kTxModeCount] = {
    {TxMode::CW, "CW",
     "A keyed carrier. The tone you hear is the receiver's, not this one's - what\n"
     "goes out is the oscillator, switched on and off with a shaped edge so it\n"
     "does not click across the band."},
    {TxMode::AM, "AM",
     "Carrier and both sidebands. The oldest mode there is, and the widest of\n"
     "these for the voice it carries."},
    {TxMode::NFM, "NFM",
     "Narrow-band FM at 2.5 kHz deviation, with the pre-emphasis a receiver's\n"
     "de-emphasis expects. What a handheld on a repeater uses."},
    {TxMode::USB, "USB",
     "Upper sideband: no carrier, one sideband, everything above the dial\n"
     "frequency. The efficient way to put a voice on HF, and what is used above\n"
     "10 MHz by convention."},
    {TxMode::LSB, "LSB",
     "Lower sideband: the same thing mirrored below the dial frequency, which is\n"
     "the convention below 10 MHz."},
};

}  // namespace

const char* txModeName(TxMode m) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= kTxModeCount) { return kModes[0].name; }
    return kModes[i].name;
}

const char* txModeCaption(TxMode m) {
    const int i = static_cast<int>(m);
    if (i < 0 || i >= kTxModeCount) { return kModes[0].caption; }
    return kModes[i].caption;
}

TxMode txModeFromIndex(int index) {
    if (index < 0 || index >= kTxModeCount) { return TxMode::CW; }
    return static_cast<TxMode>(index);
}

bool txModeFromName(const char* name, TxMode& out) {
    if (name == nullptr) { return false; }
    for (const ModeRow& row : kModes) {
        if (std::strcmp(row.name, name) == 0) {
            out = row.mode;
            return true;
        }
    }
    return false;
}

// --- the modulator ----------------------------------------------------------

Modulator::Modulator() {
    rebuildHilbert();
    rebuildPreemphasis();
    setSampleRateHz(sampleRateHz_);
}

void Modulator::setSampleRateHz(double hz) {
    if (!(hz > 0.0)) { return; }
    sampleRateHz_ = hz;
    // The ramp is a TIME, so the step per sample follows the rate. A ramp
    // measured in samples would click at one rate and drawl at another.
    const double rampSamples = std::max(1.0, kCwRampMs * 1.0e-3 * sampleRateHz_);
    envelopeStep_ = 1.0 / rampSamples;
    rebuildPreemphasis();
}

void Modulator::setMode(TxMode m) {
    if (m == mode_) { return; }
    mode_ = m;
    // A MODE CHANGE MID-TRANSMISSION IS A DISCONTINUITY, and the histories
    // belonging to the old mode are what would carry it onto the air: an SSB
    // delay line full of the last mode's audio, a phase accumulator parked
    // wherever FM left it. The ENVELOPE is deliberately kept, so switching
    // mode while keyed does not also restart the ramp.
    phaseCycles_ = 0.0;
    preLast_ = 0.0;
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    peakDeviationHz_ = 0.0;
}

void Modulator::setKeyed(bool on) { keyed_ = on; }

bool Modulator::idle() const { return !keyed_ && envelope_ <= 0.0; }

void Modulator::setPreemphasis(bool on) { preemphasis_ = on; }

void Modulator::setDeviationHz(double hz) {
    if (!(hz > 0.0)) { return; }
    deviationHz_ = hz;
}

void Modulator::reset() {
    keyed_ = false;
    envelope_ = 0.0;
    phaseCycles_ = 0.0;
    preLast_ = 0.0;
    peakDeviationHz_ = 0.0;
    std::fill(delay_.begin(), delay_.end(), 0.0f);
}

void Modulator::rebuildPreemphasis() {
    // y[n] = g * (x[n] - a*x[n-1]), a = exp(-1/(fs*tau)).
    preA_ = std::exp(-1.0 / (sampleRateHz_ * kNfmPreemphasisTau));
    // g normalises the magnitude response at kPreemphasisRefHz to 1. The
    // response of (1 - a z^-1) at w is |1 - a e^{-jw}| = sqrt(1 - 2a cos w +
    // a^2), so g is its reciprocal there.
    const double w = 2.0 * kPi * kPreemphasisRefHz / sampleRateHz_;
    const double mag = std::sqrt(1.0 - 2.0 * preA_ * std::cos(w) + preA_ * preA_);
    preG_ = (mag > 1.0e-12) ? (1.0 / mag) : 1.0;
}

void Modulator::rebuildHilbert() {
    const int n = kHilbertTaps;
    hilbert_.assign(static_cast<std::size_t>(n), 0.0f);
    delay_.assign(static_cast<std::size_t>(n), 0.0f);
    const int mid = n / 2;
    for (int i = 0; i < n; ++i) {
        const int k = i - mid;
        if (k == 0 || (k % 2) == 0) {
            // The ideal transformer's even taps are exactly zero, which is
            // also what halves the arithmetic: every other multiply is a
            // multiply by nothing.
            hilbert_[static_cast<std::size_t>(i)] = 0.0f;
            continue;
        }
        const double ideal = 2.0 / (kPi * static_cast<double>(k));
        // Blackman, for the stopband it buys: the opposite sideband is
        // suppressed by how deep the transformer's own ripple is, and a
        // rectangular truncation of 2/(pi k) ripples badly enough to be
        // audible on the wrong side.
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        const double win =
            0.42 - 0.5 * std::cos(2.0 * kPi * t) + 0.08 * std::cos(4.0 * kPi * t);
        hilbert_[static_cast<std::size_t>(i)] = static_cast<float>(ideal * win);
    }
}

float Modulator::nextEnvelope() {
    // A LINEAR RAMP THROUGH A RAISED COSINE, not a linear envelope. The
    // shaping is the point: the cosine's derivative is zero at both ends, so
    // the envelope leaves and arrives at its limits smoothly and there is no
    // corner anywhere for a spectrum to be wide about.
    if (keyed_) {
        envelope_ += envelopeStep_;
        if (envelope_ > 1.0) { envelope_ = 1.0; }
    } else {
        envelope_ -= envelopeStep_;
        if (envelope_ < 0.0) { envelope_ = 0.0; }
    }
    return static_cast<float>(0.5 - 0.5 * std::cos(kPi * envelope_));
}

void Modulator::process(const float* audio, std::size_t n, std::complex<float>* out) {
    if (audio == nullptr || out == nullptr || n == 0) { return; }
    peakDeviationHz_ = 0.0;

    switch (mode_) {
        case TxMode::CW: {
            // NO AUDIO AT ALL, and that is not an omission. A CW transmitter
            // keys its carrier; the tone a listener hears is made in their
            // receiver by beating it against a local oscillator. Feeding the
            // microphone in here would produce a modulated carrier that no
            // CW operator would recognise.
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = std::complex<float>(nextEnvelope(), 0.0f);
            }
            return;
        }
        case TxMode::AM: {
            for (std::size_t i = 0; i < n; ++i) {
                const float env = nextEnvelope();
                float a = audio[i];
                if (!(a == a)) { a = 0.0f; }
                if (a > 1.0f) { a = 1.0f; }
                if (a < -1.0f) { a = -1.0f; }
                const float v = 0.5f * (1.0f + kAmDepth * a);
                out[i] = std::complex<float>(v * env, 0.0f);
            }
            return;
        }
        case TxMode::NFM: {
            const double maxStep = deviationHz_ / sampleRateHz_;  // cycles per sample
            for (std::size_t i = 0; i < n; ++i) {
                const float env = nextEnvelope();
                double a = static_cast<double>(audio[i]);
                if (!(a == a)) { a = 0.0; }
                if (preemphasis_) {
                    const double y = preG_ * (a - preA_ * preLast_);
                    preLast_ = a;
                    a = y;
                } else {
                    preLast_ = a;
                }
                // THE LIMIT IS A CLAMP ON THE PHASE STEP, applied after the
                // pre-emphasis and not before it. Limiting the audio instead
                // would let the pre-emphasis push a loud high note straight
                // back out of the channel, which is exactly the combination
                // that makes a handheld unreadable on a repeater.
                double step = a * maxStep;
                if (step > maxStep) { step = maxStep; }
                if (step < -maxStep) { step = -maxStep; }
                const double devHz = std::fabs(step) * sampleRateHz_;
                if (devHz > peakDeviationHz_) { peakDeviationHz_ = devHz; }
                phaseCycles_ += step;
                // Wrapped by 1.0, which is exactly representable, so a long
                // transmission accumulates no phase error of its own.
                if (phaseCycles_ >= 0.5) { phaseCycles_ -= 1.0; }
                if (phaseCycles_ < -0.5) { phaseCycles_ += 1.0; }
                const double ang = 2.0 * kPi * phaseCycles_;
                out[i] = std::complex<float>(static_cast<float>(std::cos(ang)) * env,
                                             static_cast<float>(std::sin(ang)) * env);
            }
            return;
        }
        case TxMode::USB:
        case TxMode::LSB:
        case TxMode::Count:
        default:
            break;
    }

    // --- the two sidebands --------------------------------------------------
    const std::size_t taps = hilbert_.size();
    const std::size_t mid = taps / 2;
    // LSB is the conjugate of USB and nothing else: the analytic signal
    // x + jH{x} keeps only the positive frequencies, and its conjugate keeps
    // only the negative ones. One sign, one mode.
    const float qSign = (mode_ == TxMode::LSB) ? -1.0f : 1.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float env = nextEnvelope();
        float a = audio[i];
        if (!(a == a)) { a = 0.0f; }
        if (a > 1.0f) { a = 1.0f; }
        if (a < -1.0f) { a = -1.0f; }
        // Oldest first, newest last: shift and append, so delay_[mid] is the
        // sample the quadrature arm's convolution is centred on and therefore
        // the one the in-phase arm must use.
        std::memmove(delay_.data(), delay_.data() + 1, (taps - 1) * sizeof(float));
        delay_[taps - 1] = a;

        float q = 0.0f;
        for (std::size_t k = 0; k < taps; ++k) {
            // delay_ is oldest-first and hilbert_ is indexed from its own
            // start, so the convolution walks the delay line backwards.
            q += hilbert_[k] * delay_[taps - 1 - k];
        }
        // HALVED, for the reason AM is halved: an analytic signal's magnitude
        // is the envelope of the real signal it came from, so a full-scale
        // audio peak would sit exactly on the unit circle with nothing left
        // for the interpolator's overshoot.
        const float iArm = delay_[taps - 1 - mid] * 0.5f * env;
        out[i] = std::complex<float>(iArm, qSign * q * 0.5f * env);
    }
}

// --- the test tone ----------------------------------------------------------

void ToneGenerator::setSampleRateHz(double hz) {
    if (hz > 0.0) { sampleRateHz_ = hz; }
}

void ToneGenerator::setFrequencyHz(double hz) {
    if (!(hz == hz)) { return; }
    // Folded rather than refused: a tone above Nyquist aliases whatever is
    // done about it, and a control that silently produced the alias would be
    // lying about what is on the air.
    const double nyquist = sampleRateHz_ * 0.5;
    if (hz < 0.0) { hz = 0.0; }
    if (hz > nyquist) { hz = nyquist; }
    freqHz_ = hz;
}

void ToneGenerator::setLevel(float level01) {
    if (!(level01 >= 0.0f)) { level01 = 0.0f; }
    if (level01 > 1.0f) { level01 = 1.0f; }
    level_ = level01;
}

void ToneGenerator::generate(float* out, std::size_t n) {
    if (out == nullptr) { return; }
    const double step = freqHz_ / sampleRateHz_;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = level_ * static_cast<float>(std::sin(2.0 * kPi * phaseCycles_));
        phaseCycles_ += step;
        if (phaseCycles_ >= 1.0) { phaseCycles_ -= 1.0; }
    }
}

}  // namespace cascade::dsp
