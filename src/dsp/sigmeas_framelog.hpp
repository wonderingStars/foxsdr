// sigmeas_framelog.hpp - the MODEL SIDE of two Wavecom features (DEPTH-GAPS.md Part B): "per-character
// confidence for MFSK" and "showing frames with CRC errors". The decoders and the screen are plugin
// and screen-lane work (see the hand-off in docs/feature-records/sigmeas.md); this header is what they
// call.
//
// 1. PER-SYMBOL AND PER-CHARACTER CONFIDENCE from tone-energy margins.
//    A non-coherent M-ary FSK detector measures the energy E_j in each of M tone bins per symbol.
//    With noise of unit mean energy per bin and a tone of amplitude a, the likelihood of "tone i was
//    sent" is proportional to I0(2 a sqrt(E_i)) (I0: modified Bessel function; the other tones are
//    pure noise and their factor is the same for every i - the textbook non-coherent orthogonal-signal
//    detector of the digital-communications textbooks, not opened here: the envelope of the matched
//    filter is Rician for the sent tone and Rayleigh for the others; the I0 evaluation is checked
//    against std::cyl_bessel_i in the test). So the POSTERIOR of the winning tone,
//    under equal priors, is
//        P(best) = I0(2 a sqrt(E_best)) / sum_j I0(2 a sqrt(E_j)),
//    evaluated in the log domain. The noise level and a are taken from the symbol itself: the noise
//    is the mean energy of the tones that did not win, and a^2 = max(E_best / noise - 1, 1/4).
//    The result is a probability in 0..1 that this symbol was decoded right (it is 1/M-ish when the
//    energies are all alike and tends to 1 as the winner stands out). Also reported: the margin
//    between the best and second-best tone in dB and the estimated tone-to-noise ratio.
//    A CHARACTER made of k symbols is right only if all of them are: `characterConfidence` gives the
//    product of their posteriors (independent symbols) and the weakest one.
//
// 2. FRAME RECORD that keeps a frame whose CRC failed, with its flag, in a bounded log.
//    `FrameRecord` carries the bytes as received, the CRC as received and as computed, `crcOk`, a
//    time, a frequency and a mean symbol confidence. `FrameLog` is a ring of the last `capacity`
//    records plus running totals (good and bad frames ever pushed, including ones that have left the
//    ring). `setKeepBad(false)` makes it drop failed frames (counting them); the snapshot can show
//    good frames only or both. Thread-safe (one mutex), memory only, nothing written anywhere.
//    Frame bytes can be other people's traffic: the log never prints or saves them, and a host that
//    shows them follows the plugin's own display rules.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_FRAMELOG_HPP
#define CASCADE_DSP_SIGMEAS_FRAMELOG_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

// ---- confidence -----------------------------------------------------------------------------------------
struct SymbolConfidence {
    bool valid = false;
    const char* reason = why::kEmpty;
    int best = -1;
    int second = -1;
    double posterior = kNaN;      // probability that `best` is the tone that was sent
    double marginDb = kNaN;       // 10 log10 (E_best / E_second)
    double toneToNoiseDb = kNaN;  // estimated a^2, dB
};

namespace detail {

// log I0(x), x >= 0 (Abramowitz and Stegun 9.8.1 and 9.8.2).
inline double logI0(double x) {
    if (x < 3.75) {
        const double t = (x / 3.75) * (x / 3.75);
        const double i0 = 1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 +
                          t * (0.0360768 + t * 0.0045813)))));
        return std::log(i0);
    }
    const double t = 3.75 / x;
    const double poly = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 + t * (0.00916281 +
                        t * (-0.02057706 + t * (0.02635537 + t * (-0.01647633 + t * 0.00392377)))))));
    return x + std::log(poly) - 0.5 * std::log(x);
}

}  // namespace detail

inline SymbolConfidence symbolConfidence(const double* energy, std::size_t tones) {
    SymbolConfidence c;
    if (energy == nullptr || tones < 2 || tones > 256) {
        c.reason = energy == nullptr ? why::kEmpty : why::kBadParam;
        return c;
    }
    std::size_t best = 0;
    for (std::size_t j = 0; j < tones; ++j) {
        if (!std::isfinite(energy[j]) || energy[j] < 0.0) { c.reason = why::kNonFinite; return c; }
        if (energy[j] > energy[best]) { best = j; }
    }
    if (!(energy[best] > 0.0)) { c.reason = why::kNoSignal; return c; }
    std::size_t second = best == 0 ? 1 : 0;
    double sumOthers = 0.0;
    for (std::size_t j = 0; j < tones; ++j) {
        if (j == best) { continue; }
        sumOthers += energy[j];
        if (energy[j] > energy[second]) { second = j; }
    }
    const double noise = std::max(sumOthers / static_cast<double>(tones - 1), 1e-300);
    const double a2 = std::max(energy[best] / noise - 1.0, 0.25);
    const double a = std::sqrt(a2);
    // log-domain softmax of log I0(2 a sqrt(E_j / noise))
    std::vector<double> lg(tones);
    double mx = -1e300;
    for (std::size_t j = 0; j < tones; ++j) {
        lg[j] = detail::logI0(2.0 * a * std::sqrt(energy[j] / noise));
        mx = std::max(mx, lg[j]);
    }
    double den = 0.0;
    for (std::size_t j = 0; j < tones; ++j) { den += std::exp(lg[j] - mx); }
    c.best = static_cast<int>(best);
    c.second = static_cast<int>(second);
    c.posterior = std::exp(lg[best] - mx) / den;
    c.marginDb = energy[second] > 0.0 ? 10.0 * std::log10(energy[best] / energy[second]) : 300.0;
    c.toneToNoiseDb = toDb(a2);
    c.valid = true;
    c.reason = why::kOk;
    return c;
}

struct CharConfidence {
    bool valid = false;
    double product = kNaN;   // all k symbols right
    double minimum = kNaN;   // the weakest symbol
};

inline CharConfidence characterConfidence(const double* symbolPosteriors, std::size_t k) {
    CharConfidence c;
    if (symbolPosteriors == nullptr || k == 0) { return c; }
    double prod = 1.0, mn = 1.0;
    for (std::size_t i = 0; i < k; ++i) {
        const double p = symbolPosteriors[i];
        if (!std::isfinite(p) || p < 0.0 || p > 1.0) { return c; }
        prod *= p;
        mn = std::min(mn, p);
    }
    c.valid = true;
    c.product = prod;
    c.minimum = mn;
    return c;
}

// Indices of the characters whose confidence is under `threshold` (the places a display would mark).
inline std::vector<std::size_t> lowConfidence(const std::vector<double>& charConfidence, double threshold) {
    std::vector<std::size_t> out;
    if (!std::isfinite(threshold)) { return out; }
    for (std::size_t i = 0; i < charConfidence.size(); ++i) {
        if (!(charConfidence[i] >= threshold)) { out.push_back(i); }   // NaN counts as low
    }
    return out;
}

// ---- frames with CRC errors ---------------------------------------------------------------------------------------
struct FrameRecord {
    std::uint64_t seq = 0;          // assigned by the log
    double timeS = 0.0;
    double freqHz = 0.0;
    std::vector<std::uint8_t> bytes;
    bool hasCrc = true;
    std::uint32_t crcReceived = 0;
    std::uint32_t crcComputed = 0;
    bool crcOk = false;             // the flag the log keeps
    double confidence = kNaN;       // mean symbol confidence, if the decoder has one

    // crcOk is set from the two values, so a decoder cannot forget it.
    static FrameRecord withCrc(double timeS, double freqHz, std::vector<std::uint8_t> bytes,
                               std::uint32_t received, std::uint32_t computed, double confidence = kNaN) {
        FrameRecord r;
        r.timeS = timeS;
        r.freqHz = freqHz;
        r.bytes = std::move(bytes);
        r.crcReceived = received;
        r.crcComputed = computed;
        r.crcOk = received == computed;
        r.confidence = confidence;
        return r;
    }
    // A protocol with no check value: good by definition, the flag says there was nothing to check.
    static FrameRecord withoutCrc(double timeS, double freqHz, std::vector<std::uint8_t> bytes) {
        FrameRecord r;
        r.timeS = timeS;
        r.freqHz = freqHz;
        r.bytes = std::move(bytes);
        r.hasCrc = false;
        r.crcOk = true;
        return r;
    }
};

class FrameLog {
public:
    explicit FrameLog(std::size_t capacity = 256) : capacity_(capacity == 0 ? 1 : capacity) {}

    void setKeepBad(bool keep) {
        std::lock_guard<std::mutex> lk(m_);
        keepBad_ = keep;
    }
    bool keepBad() const {
        std::lock_guard<std::mutex> lk(m_);
        return keepBad_;
    }

    // Returns the sequence number given (also to a dropped bad frame: it was counted).
    std::uint64_t push(FrameRecord rec) {
        std::lock_guard<std::mutex> lk(m_);
        rec.seq = ++seq_;
        if (rec.crcOk) { ++good_; } else { ++bad_; }
        if (!rec.crcOk && !keepBad_) { return rec.seq; }
        ring_.push_back(std::move(rec));
        while (ring_.size() > capacity_) { ring_.pop_front(); }
        return seq_;
    }

    // Copy of the held records, oldest first; includeBad = false shows good frames only.
    std::vector<FrameRecord> snapshot(bool includeBad = true) const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<FrameRecord> out;
        out.reserve(ring_.size());
        for (const FrameRecord& r : ring_) {
            if (includeBad || r.crcOk) { out.push_back(r); }
        }
        return out;
    }

    struct Counts {
        std::uint64_t goodTotal = 0;   // ever pushed
        std::uint64_t badTotal = 0;    // ever pushed, kept or dropped
        std::size_t held = 0;
        std::size_t heldBad = 0;
    };
    Counts counts() const {
        std::lock_guard<std::mutex> lk(m_);
        Counts c;
        c.goodTotal = good_;
        c.badTotal = bad_;
        c.held = ring_.size();
        for (const FrameRecord& r : ring_) { if (!r.crcOk) { ++c.heldBad; } }
        return c;
    }

    // Empties the ring and the totals.
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        ring_.clear();
        good_ = bad_ = 0;
        seq_ = 0;
    }

    std::size_t capacity() const { return capacity_; }

private:
    mutable std::mutex m_;
    std::deque<FrameRecord> ring_;
    std::size_t capacity_;
    bool keepBad_ = true;
    std::uint64_t seq_ = 0, good_ = 0, bad_ = 0;
};

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_FRAMELOG_HPP
