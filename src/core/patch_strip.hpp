// patch_strip.hpp - one channel of a patch: mix it to DC, narrow it, decimate
// it, and demodulate it.
//
//     mix to DC  ->  low-pass and decimate  ->  AM envelope or FM discriminator
//
// One of these per Channel node. It is pure DSP with no threads, no audio
// device and no graph, so tests/test_patch_strip.cpp can drive it with signals
// whose answer is known rather than judged - a tone at a known offset, a
// neighbour that must be rejected, a carrier that must not leak.
//
// WHY IT IS SEPARATE FROM EVERYTHING ELSE. The wiring that will eventually run
// these on the audio thread is the riskiest change in this project: a rebuild
// while sound is playing is the shape of fault that has twice produced a hang
// or a silently dead stream here. Getting the arithmetic right FIRST, where it
// can be driven at any speed and inspected, means that when the threading does
// land the only new question is the threading.
//
// THE FILTER IS NOT OPTIONAL, and it is the whole reason this is more than a
// multiply. Mixing a channel to DC brings it to baseband but leaves every
// other channel in the capture sitting beside it; without a low-pass before
// the decimation they all fold on top of each other and the decode hears a
// crowd. The tests assert a neighbour is rejected rather than merely that the
// wanted channel survives, because a strip with no filter passes the second.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PATCH_STRIP_HPP
#define CASCADE_CORE_PATCH_STRIP_HPP

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::core::patch {

enum class Demod : std::uint8_t { Am, Fm };

// Taps per unit of decimation. Eight is the workbench's figure and it is a
// transition width, not a guess: fewer and the skirt is wide enough to let a
// neighbour through, more and the cost grows for stopband nobody hears.
inline constexpr std::size_t kTapsPerDecimation = 8;
inline constexpr std::size_t kMaxTaps = 1024;

// How often the oscillator is renormalised. A recursive NCO is one complex
// multiply per sample instead of a sine and a cosine, and it drifts off the
// unit circle as the rounding accumulates - slowly, but a strip runs for
// hours. Every few thousand samples costs nothing and bounds the error.
inline constexpr std::size_t kNcoRenormInterval = 4096;

class Strip {
public:
    // `offsetHz` is signed and measured from the radio's centre.
    void configure(double offsetHz, double inRateHz, unsigned decimation) {
        inRateHz_ = inRateHz;
        decimation_ = std::max(1u, decimation);
        offsetHz_ = offsetHz;

        // The oscillator turns the channel DOWN to DC, so its rate is the
        // NEGATIVE of the offset.
        const double w = (inRateHz > 0.0) ? (-2.0 * 3.14159265358979323846 * offsetHz / inRateHz)
                                          : 0.0;
        step_ = std::complex<double>(std::cos(w), std::sin(w));

        buildTaps();
        reset();
    }

    void reset() {
        phase_ = std::complex<double>(1.0, 0.0);
        sinceRenorm_ = 0;
        history_.assign(taps_.size(), std::complex<float>(0.0f, 0.0f));
        pos_ = 0;
        counter_ = 0;
        prev_ = std::complex<float>(0.0f, 0.0f);
        dcState_ = 0.0f;
        havePrev_ = false;
    }

    double outRateHz() const {
        return (decimation_ > 0) ? inRateHz_ / static_cast<double>(decimation_) : 0.0;
    }

    std::size_t tapCount() const { return taps_.size(); }

    // The filter's gain at DC: the sum of its taps. Exposed because it is
    // the one property of the filter a test can assert directly, and
    // because a strip whose gain is not 1 quietly changes the level of
    // everything it passes - two strips on one signal would then disagree
    // about how strong it is.
    double dcGain() const {
        double sum = 0.0;
        for (const float t : taps_) { sum += static_cast<double>(t); }
        return sum;
    }

    // Consumes `n` samples and APPENDS the demodulated audio to `out`. Appends
    // rather than overwrites so a caller can accumulate several blocks without
    // a second buffer.
    void process(const std::complex<float>* in, std::size_t n, Demod mode,
                 std::vector<float>& out) {
        if (in == nullptr || taps_.empty()) { return; }

        for (std::size_t i = 0; i < n; ++i) {
            // --- mix to DC ---------------------------------------------------
            const std::complex<float> mixed(
                static_cast<float>(in[i].real() * phase_.real() - in[i].imag() * phase_.imag()),
                static_cast<float>(in[i].real() * phase_.imag() + in[i].imag() * phase_.real()));
            phase_ *= step_;
            if (++sinceRenorm_ >= kNcoRenormInterval) {
                sinceRenorm_ = 0;
                const double m = std::abs(phase_);
                if (m > 0.0) { phase_ /= m; }
            }

            history_[pos_] = mixed;
            pos_ = (pos_ + 1 == history_.size()) ? 0 : pos_ + 1;

            if (++counter_ < decimation_) { continue; }
            counter_ = 0;

            // --- low-pass, at the decimated rate only ------------------------
            // The dot product is computed once per OUTPUT sample rather than
            // once per input sample: everything it would produce in between is
            // thrown away by the decimation, so computing it is work with no
            // effect on the answer.
            std::complex<float> acc(0.0f, 0.0f);
            std::size_t idx = pos_;
            for (std::size_t t = 0; t < taps_.size(); ++t) {
                acc += history_[idx] * taps_[t];
                idx = (idx + 1 == history_.size()) ? 0 : idx + 1;
            }

            out.push_back(demodulate(acc, mode));
        }
    }

private:
    float demodulate(std::complex<float> z, Demod mode) {
        float v = 0.0f;
        if (mode == Demod::Am) {
            v = std::abs(z);
        } else {
            // The angle the vector turned since the last sample IS the
            // instantaneous frequency. arg(z * conj(prev)) rather than a
            // difference of two arg() calls, which wraps at +/-pi and puts a
            // click in the audio every time it does.
            if (havePrev_) {
                const std::complex<float> d = z * std::conj(prev_);
                v = std::atan2(d.imag(), d.real());
            }
            prev_ = z;
            havePrev_ = true;
        }

        // A one-pole DC blocker. AM's envelope is all positive and would
        // otherwise carry a large offset into the audio; FM's discriminator
        // sits off zero whenever the channel is not exactly centred.
        //
        // THE CORNER IS DERIVED FROM THE OUTPUT RATE, not written as a
        // constant. At a fixed coefficient the settling time changes with the
        // rate, and an earlier version of this arithmetic elsewhere in the
        // product took 42 ms to settle against a 1.3 ms preamble - it removed
        // the offset long after the thing being decoded had gone past.
        const double fc = 80.0;
        const double r = outRateHz();
        const float a = (r > 0.0) ? static_cast<float>(
                                        1.0 - std::exp(-2.0 * 3.14159265358979323846 * fc / r))
                                  : 0.0f;
        dcState_ += a * (v - dcState_);
        return v - dcState_;
    }

    void buildTaps() {
        std::size_t n = kTapsPerDecimation * decimation_;
        n = std::clamp<std::size_t>(n, 8, kMaxTaps);
        if ((n & 1u) == 0u) { ++n; }   // odd, so there is a true centre tap

        taps_.assign(n, 0.0f);
        // Cut at half the OUTPUT rate: that is the widest a channel can be
        // and still survive the decimation without folding.
        const double cutoff = 0.5 / static_cast<double>(decimation_);
        const double centre = 0.5 * static_cast<double>(n - 1);
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double k = static_cast<double>(i) - centre;
            const double x = 2.0 * 3.14159265358979323846 * cutoff * k;
            const double sinc = (std::fabs(k) < 1e-9) ? (2.0 * cutoff)
                                                      : (std::sin(x) / (3.14159265358979323846 * k));
            // Hamming: a -43 dB first sidelobe, which is what keeps the
            // neighbour out rather than merely quieter.
            const double win = 0.54 - 0.46 * std::cos(2.0 * 3.14159265358979323846 *
                                                      static_cast<double>(i) /
                                                      static_cast<double>(n - 1));
            const double v = sinc * win;
            taps_[i] = static_cast<float>(v);
            sum += v;
        }
        // Unity gain at DC, so a strip does not change the level of what it
        // passes and two strips on one signal agree with each other.
        if (std::fabs(sum) > 1e-12) {
            for (float& t : taps_) { t = static_cast<float>(t / sum); }
        }
    }

    double inRateHz_ = 0.0;
    double offsetHz_ = 0.0;
    unsigned decimation_ = 1;

    std::complex<double> phase_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
    std::size_t sinceRenorm_ = 0;

    std::vector<float> taps_;
    std::vector<std::complex<float>> history_;
    std::size_t pos_ = 0;
    unsigned counter_ = 0;

    std::complex<float> prev_{0.0f, 0.0f};
    bool havePrev_ = false;
    float dcState_ = 0.0f;
};

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_STRIP_HPP
