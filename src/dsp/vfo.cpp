// VFO channelizer: NCO mix to baseband, anti-alias low-pass, integer decimate.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/vfo.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>

namespace cascade::dsp {

namespace {
// Internal processing chunk. Bounds the scratch buffers regardless of how
// large a block the caller hands in, so memory stays O(kChunk) while the
// per-call loop overhead stays negligible against the FIR work.
constexpr std::size_t kChunk = 8192;

// fred harris' rule of thumb N ~= A / (22 * df), with A ~= 92 dB - the
// sidelobe floor of the 4-term Blackman-Harris window, i.e. the stopband
// depth this design can reach. Floor at 11 taps (below that the formula's
// small-N error dominates) and cap at 16383 as a memory/CPU sanity bound for
// absurd decimations. windowedSincLowpass requires an odd length (symmetric
// peak, integer group delay); both clamp bounds are odd so the OR cannot
// exceed the cap.
std::size_t tapsForTransition(double transNorm) {
    std::size_t n = static_cast<std::size_t>(92.0 / (22.0 * transNorm)) + 1;
    n = std::clamp<std::size_t>(n, 11, 16383);
    return n | 1u;
}

// THE SINGLE-STAGE CHANNEL FILTER, exactly as it has always been designed,
// for a filter running at `rateHz` and decimating to `channelRate`.
//   - passband edge = bw/2, the -6 dB cutoff of a windowed-sinc design,
//     normalised to the rate the filter RUNS at (it runs before decimation);
//   - stopband edge = the channel Nyquist: every frequency beyond
//     channelRate/2 folds somewhere into the channel after decimation, so the
//     whole channel - not just the passband - is protected from aliases.
std::vector<float> channelTaps(double rateHz, double channelRate, double bw) {
    const double cutoffNorm = 0.5 * bw / rateHz;
    const double stopNorm = 0.5 * channelRate / rateHz;
    const double transNorm = stopNorm - cutoffNorm;  // >= 0.05/decim by clamp
    return windowedSincLowpass(tapsForTransition(transNorm), cutoffNorm,
                               WindowType::BlackmanHarris);
}

// The prime factors of n, largest first.
std::vector<unsigned> primeFactorsDescending(unsigned n) {
    std::vector<unsigned> f;
    for (unsigned p = 2; p * p <= n; ++p) {
        while (n % p == 0) {
            f.push_back(p);
            n /= p;
        }
    }
    if (n > 1) { f.push_back(n); }
    std::sort(f.begin(), f.end(), std::greater<unsigned>());
    return f;
}
}  // namespace

Vfo::Vfo(double inputRateHz, unsigned decimation, double bandwidthHz)
    : inputRate_(inputRateHz),
      // Folding 0 to 1 keeps the arithmetic below finite even if the release
      // build skips the assert; 0 has no meaningful interpretation anyway.
      decimFactor_(decimation == 0 ? 1u : decimation),
      bandwidth_(clampBandwidth(bandwidthHz)),
      stages_(designStages(bandwidth_)) {
    assert(inputRateHz > 0.0);
    assert(decimation >= 1);
}

double Vfo::clampBandwidth(double bw) const {
    const double channelRate = inputRate_ / static_cast<double>(decimFactor_);
    // Upper bound 0.9 * channelRate: leaves at least 0.05 * channelRate of
    // transition band on each side before the channel Nyquist, which a finite
    // filter needs to actually attenuate the aliases. Lower bound keeps the
    // windowed-sinc cutoff strictly positive (design precondition).
    return std::clamp(bw, 0.01 * channelRate, 0.9 * channelRate);
}

// WHY STAGES, AND WHY ONLY ABOVE 4 MS/s.
//
// A single windowed sinc at the input rate needs a length proportional to
// that rate (its transition band, bw/2 to the channel Nyquist, is fixed in
// hertz), and it runs once per channel sample, so its cost per INPUT sample is
// the same ~33 multiply-adds at every rate - and its cost per SECOND grows
// with the rate. At 10 MS/s it is 1673 taps, a quarter of a CPU-second for
// every second of signal on the fastest desktop processor there is, and on a
// slower machine the receiver falls behind and the sound breaks up. That is
// what an Airspy R2 owner heard at 10 MS/s and not at 2.5.
//
// A cascade does the same job for a fraction of the work. The decimation is
// split into its prime factors, largest first, and every stage but the last
// only has to keep aliases OUT OF THE CHANNEL - not out of everything below
// its own output Nyquist - so its transition band is wide and it is short:
//   stage i, rate r_in -> r_out = r_in / d_i:
//     passband edge bw/2 (flat for the channel's own passband), stopband edge
//     r_out - channelRate/2 (the first frequency that folds into
//     [-channelRate/2, channelRate/2]), 92 dB at the Blackman-Harris floor.
// The LAST stage is the channel filter proper, designed exactly as the single
// stage always was but at its own, much lower input rate. At 10 MS/s / 50
// that is 5 x 5 x 2: 23 + 39 + 67 taps, 7.5 multiply-adds per input sample
// instead of 33.5. The output grid is unchanged (sample 0, D, 2D ... of the
// input), because every stage emits on its own grid's phase 0.
//
// Below kStagedMinInputRateHz nothing changes at all: the measured headroom
// there is several times real time, and every radio that runs there - an
// RTL-SDR, an Airspy at 2.5 or 3 MS/s - keeps exactly the filter it had.
std::vector<FirDecimator> Vfo::designStages(double bw) const {
    const double channelRate = inputRate_ / static_cast<double>(decimFactor_);
    std::vector<FirDecimator> stages;
    std::vector<unsigned> factors;
    if (inputRate_ >= kStagedMinInputRateHz) { factors = primeFactorsDescending(decimFactor_); }
    if (factors.size() < 2) {
        stages.emplace_back(channelTaps(inputRate_, channelRate, bw), decimFactor_);
        return stages;
    }
    // Every factor but the smallest becomes an alias-guard stage; the
    // smallest (the last, after the descending sort) is the channel stage.
    double rate = inputRate_;
    for (std::size_t i = 0; i + 1 < factors.size(); ++i) {
        const double out = rate / static_cast<double>(factors[i]);
        const double passHz = 0.5 * bw;
        const double stopHz = out - 0.5 * channelRate;
        const double cutoffNorm = 0.5 * (passHz + stopHz) / rate;
        // Cutoff to stop edge, the same half-width channelTaps hands the rule:
        // a Blackman-Harris windowed sinc falls from unity to its floor over
        // about +/-4/N around the cutoff, which is what 92/22 ~= 4.2 buys.
        // Handing it the FULL width instead (tried first) left a -32 dB alias
        // at 10 MS/s, which the alias sweep in tests/test_vfo.cpp caught.
        const double transNorm = stopHz / rate - cutoffNorm;
        stages.emplace_back(windowedSincLowpass(tapsForTransition(transNorm), cutoffNorm,
                                                WindowType::BlackmanHarris),
                            factors[i]);
        rate = out;
    }
    stages.emplace_back(channelTaps(rate, channelRate, bw), factors.back());
    return stages;
}

void Vfo::setOffsetHz(double offset) {
    offset_ = offset;
    // Down-conversion: multiply by e^{-j 2 pi (offset/fs) n} so the content at
    // (center + offset) lands at DC. Nco::mix translates by +f, hence -offset.
    // Nco::setFrequency leaves the phase accumulator alone, which is what
    // makes retuning phase-continuous.
    nco_.setFrequency(-offset / inputRate_);
}

double Vfo::offsetHz() const { return offset_; }

void Vfo::setBandwidthHz(double bw) {
    bandwidth_ = clampBandwidth(bw);
    // Fresh FirDecimators: new taps, zero history, grid phase 0. See the
    // header for why history is not carried across a redesign.
    stages_ = designStages(bandwidth_);
}

double Vfo::channelRateHz() const {
    return inputRate_ / static_cast<double>(decimFactor_);
}

double Vfo::macsPerInputSample() const {
    // Stage i runs its taps once per ITS output, and emits one output for
    // every (d_1 * ... * d_i) input samples.
    double macs = 0.0;
    double stride = 1.0;
    for (const FirDecimator& s : stages_) {
        stride *= static_cast<double>(s.decimation());
        macs += static_cast<double>(s.taps().size()) / stride;
    }
    return macs;
}

std::size_t Vfo::process(const std::complex<float>* in, std::size_t n,
                         std::complex<float>* out, std::size_t outCap) {
    std::size_t produced = 0;
    std::size_t pos = 0;
    stageBufs_.resize(stages_.size());
    while (pos < n) {
        const std::size_t m = std::min(kChunk, n - pos);
        // Mix into a scratch buffer rather than in-place: the caller's input
        // is const, and Vfo must not mutate it.
        mixBuf_.resize(m);
        nco_.mix(in + pos, mixBuf_.data(), m);

        // Filter+decimate through every stage into scratch sized for the
        // worst case, then copy out what fits. Every stage always consumes
        // its whole input even when outCap is exhausted — NCO phase, filter
        // history, and grid phase must advance over every input sample or
        // later calls would see a stream with a hole in it.
        const std::complex<float>* stageIn = mixBuf_.data();
        std::size_t stageN = m;
        for (std::size_t s = 0; s < stages_.size(); ++s) {
            std::vector<std::complex<float>>& buf = stageBufs_[s];
            buf.resize(stages_[s].outputCapacity(stageN));
            stageN = stages_[s].process(stageIn, stageN, buf.data());
            stageIn = buf.data();
        }
        const std::size_t take = std::min(stageN, outCap - produced);
        std::copy(stageIn, stageIn + take, out + produced);
        produced += take;
        pos += m;
    }
    return produced;
}

void Vfo::reset() {
    nco_.reset();  // phase to 0; frequency (offset) retained
    for (FirDecimator& s : stages_) {
        s.reset();  // history to zeros, grid back to sample 0
    }
}

}  // namespace cascade::dsp
