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
// TWO OPTIONAL STAGES (2026-10, for the AIRBAND monitor), both OFF unless a
// caller turns them on, so a patch built before them sounds exactly as it did:
//
//   setChannelFilter(bw)  a second low-pass AT THE OUTPUT RATE, `bw` wide
//       (two-sided). The first filter only stops the decimation folding, and
//       at ~48 kHz out it passes +/-24 kHz - two 25 kHz airband channels, or
//       five 8.33 kHz ones. Narrowing it at the low rate costs tens of taps
//       instead of thousands. The squelch and an I/Q decoder see the
//       narrowed channel, so a neighbour cannot open this channel's squelch.
//   setAmNormalise(true)  AM audio divided by the carrier it rides on, so a
//       strong tower and a weak aircraft play at the same loudness - the job
//       the receiver's AGC does for its one channel, done per strip because a
//       monitor mixes several.
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
        buildChannelTaps();
        // The carrier level's attack and release (see demodulate()), as
        // one-pole coefficients at the output rate.
        const double r = outRateHz();
        attackC_ = (r > 0.0) ? static_cast<float>(1.0 - std::exp(-1.0 / (0.010 * r))) : 0.0f;
        releaseC_ = (r > 0.0) ? static_cast<float>(1.0 - std::exp(-1.0 / (0.300 * r))) : 0.0f;
        reset();
    }

    // The second, narrow filter (see the header comment). `bandwidthHz` is
    // two-sided; 0, or anything not narrower than the output rate already
    // passes, turns it off. Call after configure() - a reconfigure keeps the
    // bandwidth and redesigns the filter for the new rate.
    void setChannelFilter(double bandwidthHz) {
        channelBwHz_ = (bandwidthHz > 0.0 && std::isfinite(bandwidthHz)) ? bandwidthHz : 0.0;
        buildChannelTaps();
        chanHistory_.assign(chanTaps_.size(), std::complex<float>(0.0f, 0.0f));
        chanPos_ = 0;
    }
    std::size_t channelFilterTaps() const { return chanTaps_.size(); }

    void setAmNormalise(bool on) { amNormalise_ = on; }
    // What setAmNormalise last said (0.99.66: the airband monitor sets it per
    // channel, and a test reads it back).
    bool amNormalise() const { return amNormalise_; }

    void reset() {
        phase_ = std::complex<double>(1.0, 0.0);
        sinceRenorm_ = 0;
        history_.assign(taps_.size(), std::complex<float>(0.0f, 0.0f));
        pos_ = 0;
        counter_ = 0;
        prev_ = std::complex<float>(0.0f, 0.0f);
        dcState_ = 0.0f;
        carrier_ = 0.0f;
        havePrev_ = false;
        chanHistory_.assign(chanTaps_.size(), std::complex<float>(0.0f, 0.0f));
        chanPos_ = 0;
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
    //
    // `iqOut`, when given, also receives the channel itself - the filtered,
    // decimated complex baseband with this channel's frequency at DC, one
    // sample per audio sample. That is what an I/Q decoder plugin hung off a
    // channel is fed: the channel's own slice of the band, tuned, rather than
    // the whole capture at the radio's centre.
    void process(const std::complex<float>* in, std::size_t n, Demod mode,
                 std::vector<float>& out,
                 std::vector<std::complex<float>>* iqOut = nullptr) {
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

            // --- the narrow channel filter, when one is set ------------------
            if (!chanTaps_.empty()) {
                chanHistory_[chanPos_] = acc;
                chanPos_ = (chanPos_ + 1 == chanHistory_.size()) ? 0 : chanPos_ + 1;
                std::complex<float> nar(0.0f, 0.0f);
                std::size_t k = chanPos_;
                for (std::size_t t = 0; t < chanTaps_.size(); ++t) {
                    nar += chanHistory_[k] * chanTaps_[t];
                    k = (k + 1 == chanHistory_.size()) ? 0 : k + 1;
                }
                acc = nar;
            }

            if (iqOut != nullptr) { iqOut->push_back(acc); }
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
        float y = v - dcState_;

        // CARRIER NORMALISATION (AM only, when asked). The envelope rides on
        // the carrier, so dividing the audio by the carrier's level turns a
        // modulation depth into a loudness: 50% modulation plays the same
        // from a tower 5 km away as from an aircraft 80 km away.
        //
        // FAST ATTACK, SLOW RELEASE, like any AGC. The level follows a rise
        // in 10 ms and a fall over 300 ms. A symmetric 0.3 s average was the
        // first version, and it started every call from the noise floor: the
        // opening half-second of each transmission - the callsign - came out
        // several times too loud and clipped. The fast attack also follows
        // modulation peaks a little, so the level sits between the carrier
        // and its peak and a full-depth call is a little quieter than 1;
        // what matters is that it is the SAME for every carrier strength.
        // Clamped, because a carrier fading to nothing would otherwise
        // divide the noise up to full scale between squelch decisions.
        if (amNormalise_ && mode == Demod::Am) {
            carrier_ += ((v > carrier_) ? attackC_ : releaseC_) * (v - carrier_);
            const float floorLevel = 1e-6f;
            y = (carrier_ > floorLevel) ? y / carrier_ : 0.0f;
            y = std::clamp(y, -2.0f, 2.0f);
        }
        return y;
    }

    // The channel filter's taps, at the OUTPUT rate. Off (no taps) when no
    // bandwidth is set or the bandwidth is not narrower than the output rate.
    void buildChannelTaps() {
        chanTaps_.clear();
        const double r = outRateHz();
        if (!(channelBwHz_ > 0.0) || !(r > 0.0) || channelBwHz_ >= r) { return; }
        // Transition about a third of the bandwidth, which is what puts an
        // 8.33 kHz neighbour's speech (5-11 kHz from a 6 kHz channel's centre)
        // into the stopband; floored so a very narrow channel does not ask for
        // hundreds of taps.
        const double transition = std::clamp(channelBwHz_ * 0.35, 1000.0, 8000.0);
        std::size_t n = static_cast<std::size_t>(std::ceil(3.3 * r / transition));
        n = std::clamp<std::size_t>(n, 15, 255);
        if ((n & 1u) == 0u) { ++n; }
        chanTaps_.assign(n, 0.0f);
        // THE TRANSITION STARTS AT THE CHANNEL'S EDGE, not centred on it: a
        // windowed sinc is -6 dB at its cutoff, so a cutoff at bw/2 left a
        // 6 kHz (8.33) channel flat only to about 2 kHz and its voices muffled
        // (review, 2026-10-01). Half the transition above the edge keeps the
        // whole voice band and still puts an 8.33 kHz neighbour's speech -
        // from 5.3 kHz out - in the stopband.
        const double cutHz = std::min(0.5 * channelBwHz_ + 0.5 * transition, 0.45 * r);
        const double cutoff = cutHz / r;
        const double centre = 0.5 * static_cast<double>(n - 1);
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double k = static_cast<double>(i) - centre;
            const double x = 2.0 * 3.14159265358979323846 * cutoff * k;
            const double sinc = (std::fabs(k) < 1e-9) ? (2.0 * cutoff)
                                                      : (std::sin(x) / (3.14159265358979323846 * k));
            const double win = 0.54 - 0.46 * std::cos(2.0 * 3.14159265358979323846 *
                                                      static_cast<double>(i) /
                                                      static_cast<double>(n - 1));
            chanTaps_[i] = static_cast<float>(sinc * win);
            sum += sinc * win;
        }
        if (std::fabs(sum) > 1e-12) {
            for (float& t : chanTaps_) { t = static_cast<float>(t / sum); }
        }
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

    double channelBwHz_ = 0.0;
    std::vector<float> chanTaps_;
    std::vector<std::complex<float>> chanHistory_;
    std::size_t chanPos_ = 0;
    bool amNormalise_ = false;
    float carrier_ = 0.0f;
    float attackC_ = 0.0f;
    float releaseC_ = 0.0f;
};

}  // namespace cascade::core::patch

#endif  // CASCADE_CORE_PATCH_STRIP_HPP
