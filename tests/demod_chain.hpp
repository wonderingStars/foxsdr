// demod_chain.hpp - the receive chain's demod phase, built from the SAME classes in the SAME order as core/pipeline.cpp (processAudioBlock, the
// lines from `vfo_.process` to the host audio's resamplers), without the Pipeline's threads, locks and sinks. Used by tools/bench_demod_chain
// (the timing) and tests/test_dsp_equivalence.cpp (the output hashes and the old-against-new comparisons).
//
// What it taps are the four things outside the DSP thread that depend on what this phase computes:
//   ifIq        the VFO output: what the IF recorder gets (pipeline.cpp: ifRecorder_->writeIq(chanBuf_...))
//   mpx192      the discriminator output of a WFM channel resampled to 192 kHz: what the multiplex recorder gets (mpxResampler_)
//   decoder48   0.5 (L + R) at 48 kHz, before AGC and squelch: what every audio decoder plugin gets (resamplerD_)
//   audioL/R    the left and right channels at 48 kHz (resampler_ / resamplerR_): what the listener's chain starts from
// The AGC, squelch, notches, noise reduction and equaliser come after and are not in this phase.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dsp/demod.hpp"
#include "dsp/dsp_substep.hpp"
#include "dsp/rds.hpp"
#include "dsp/resampler.hpp"
#include "dsp/squelch.hpp"
#include "dsp/stereo_fm.hpp"
#include "dsp/vfo.hpp"

namespace demodchain {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kFmDeviationHz = 75000.0;   // pipeline.cpp kFmDeviationHz
constexpr double kMeterAlpha = 0.0005f;      // pipeline.cpp kMeterAlpha
constexpr double kAudioRateHz = 48000.0;
constexpr double kMpxRateHz = 192000.0;
constexpr double kSidebandChannelFactor = 4.0;

struct Feeds {
    std::vector<std::complex<float>> ifIq;
    std::vector<float> mpx192;
    std::vector<float> decoder48;
    std::vector<float> audioL;
    std::vector<float> audioR;
};

// FNV-1a over the bit patterns of a float run: a hash a human can compare between two lines of a log.
inline std::uint64_t hashBits(const float* p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n * sizeof(float); ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
inline std::uint64_t hashBits(const std::complex<float>* p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    return hashBits(reinterpret_cast<const float*>(p), 2 * n, h);
}

// The channel decimation for an input rate: round(rate / 200 kHz), at least 1 (pipeline.cpp decimationForInputRate for the rates the tests use:
// 2 MS/s -> 10, 1 MS/s -> 5, 500 kS/s -> 2).
inline unsigned decimationFor(double rateHz) { return static_cast<unsigned>(std::max(1.0, std::floor(rateHz / 200000.0 + 0.5))); }

// The default VFO bandwidth of a mode, as the receiver starts it.
inline double defaultBandwidthHz(cascade::dsp::DemodMode m) {
    using cascade::dsp::DemodMode;
    switch (m) {
        case DemodMode::WFM: return 150000.0;
        case DemodMode::NFM: return 12500.0;
        default: return 10000.0;   // AM, DSB, RAW
    }
}

class Chain {
public:
    // keep: store the four taps (the equivalence tests); false = take them and drop them (the benchmark).
    Chain(double inputRateHz, cascade::dsp::DemodMode mode, double offsetHz, bool keep, double ssbBandwidthHz = 3000.0)
        : inputRate_(inputRateHz),
          decim_(decimationFor(inputRateHz)),
          chanRate_(inputRateHz / static_cast<double>(decimationFor(inputRateHz))),
          vfo_(inputRateHz, decimationFor(inputRateHz), 150000.0),
          demod_(inputRateHz / static_cast<double>(decimationFor(inputRateHz))),
          meter_(static_cast<float>(kMeterAlpha)),
          resampler_(static_cast<unsigned>(kAudioRateHz + 0.5), static_cast<unsigned>(chanRate_ + 0.5)),
          resamplerR_(static_cast<unsigned>(kAudioRateHz + 0.5), static_cast<unsigned>(chanRate_ + 0.5)),
          resamplerD_(static_cast<unsigned>(kAudioRateHz + 0.5), static_cast<unsigned>(chanRate_ + 0.5)),
          mpxResampler_(static_cast<unsigned>(kMpxRateHz), static_cast<unsigned>(chanRate_ + 0.5)),
          stereo_(chanRate_),
          rds_(chanRate_),
          keep_(keep) {
        using cascade::dsp::DemodMode;
        mode_ = mode;
        demod_.setMode(mode);
        const bool wfm = mode == DemodMode::WFM;
        demod_.setDeemphasisUs(wfm ? 0.0 : 50.0);
        stereo_.setDeemphasisUs(50.0);
        const bool sideband = mode == DemodMode::USB || mode == DemodMode::LSB || mode == DemodMode::CW;
        if (sideband) {
            demod_.setSsbBandwidthHz(ssbBandwidthHz);
            vfo_.setBandwidthHz(kSidebandChannelFactor * demod_.ssbBandwidthHz());
        } else {
            vfo_.setBandwidthHz(defaultBandwidthHz(mode));
        }
        vfo_.setOffsetHz(offsetHz);
        fmScale_ = static_cast<float>(0.5 * chanRate_ / (kTwoPi * kFmDeviationHz));
    }

    double channelRateHz() const { return chanRate_; }
    cascade::dsp::StereoFm& stereo() { return stereo_; }
    cascade::dsp::RdsDecoder& rds() { return rds_; }
    const Feeds& feeds() const { return feeds_; }
    Feeds& feeds() { return feeds_; }
    std::size_t blocks() const { return blocks_; }

    // One block of input-rate samples through the chain (pipeline.cpp processAudioBlock, from the VFO to the host audio's resamplers).
    void process(const std::complex<float>* in, std::size_t n) {
        using cascade::dsp::DemodMode;
        ++blocks_;
        chanBuf_.resize(n / decim_ + 1);
        std::size_t m = 0;
        {
            DSP_SUBSTEP("vfo.total");
            m = vfo_.process(in, n, chanBuf_.data(), chanBuf_.size());
        }
        if (m == 0) { return; }
        meter_.process(chanBuf_.data(), m);
        if (keep_) { feeds_.ifIq.insert(feeds_.ifIq.end(), chanBuf_.begin(), chanBuf_.begin() + static_cast<std::ptrdiff_t>(m)); }
        audioBuf_.resize(m);
        {
            DSP_SUBSTEP("demod.total");
            demod_.process(chanBuf_.data(), m, audioBuf_.data());
        }
        const bool wfm = mode_ == DemodMode::WFM;
        if (wfm && keep_) {
            const std::size_t cap = mpxResampler_.maxOut(m);
            mpxBuf_.resize(cap);
            const std::size_t got = mpxResampler_.process(audioBuf_.data(), m, mpxBuf_.data(), cap);
            feeds_.mpx192.insert(feeds_.mpx192.end(), mpxBuf_.begin(), mpxBuf_.begin() + static_cast<std::ptrdiff_t>(got));
        }
        if (wfm) {
            DSP_SUBSTEP("rds.total");
            rds_.process(audioBuf_.data(), m);
        }
        if (mode_ == DemodMode::NFM || wfm) {
            for (std::size_t i = 0; i < m; ++i) { audioBuf_[i] *= fmScale_; }
        }
        leftBuf_.resize(m);
        rightBuf_.resize(m);
        if (wfm) {
            DSP_SUBSTEP("stereo.total");
            stereo_.process(audioBuf_.data(), m, leftBuf_.data(), rightBuf_.data());
        } else {
            std::copy(audioBuf_.begin(), audioBuf_.begin() + static_cast<std::ptrdiff_t>(m), leftBuf_.begin());
            std::copy(audioBuf_.begin(), audioBuf_.begin() + static_cast<std::ptrdiff_t>(m), rightBuf_.begin());
        }
        // the decoder tap: mono mix, resampled to 48 kHz, before the AGC
        {
            DSP_SUBSTEP("tap.resampler");
            preAgc_.resize(m);
            for (std::size_t i = 0; i < m; ++i) { preAgc_[i] = 0.5f * (leftBuf_[i] + rightBuf_[i]); }
            const std::size_t dcap = resamplerD_.maxOut(m);
            decoderBuf_.resize(dcap);
            const std::size_t dk = resamplerD_.process(preAgc_.data(), m, decoderBuf_.data(), dcap);
            if (keep_) { feeds_.decoder48.insert(feeds_.decoder48.end(), decoderBuf_.begin(), decoderBuf_.begin() + static_cast<std::ptrdiff_t>(dk)); }
        }
        // the host audio's two output resamplers (the rest of that chain is not what this phase changes)
        {
            DSP_SUBSTEP("host.resamplers");
            const std::size_t cap = resampler_.maxOut(m);
            outL_.resize(cap);
            outR_.resize(cap);
            const std::size_t kl = resampler_.process(leftBuf_.data(), m, outL_.data(), cap);
            const std::size_t kr = resamplerR_.process(rightBuf_.data(), m, outR_.data(), cap);
            if (keep_) {
                feeds_.audioL.insert(feeds_.audioL.end(), outL_.begin(), outL_.begin() + static_cast<std::ptrdiff_t>(kl));
                feeds_.audioR.insert(feeds_.audioR.end(), outR_.begin(), outR_.begin() + static_cast<std::ptrdiff_t>(kr));
            }
        }
    }

private:
    double inputRate_;
    unsigned decim_;
    double chanRate_;
    cascade::dsp::Vfo vfo_;
    cascade::dsp::Demodulator demod_;
    cascade::dsp::PowerMeter meter_;
    cascade::dsp::RationalResampler resampler_, resamplerR_, resamplerD_, mpxResampler_;
    cascade::dsp::StereoFm stereo_;
    cascade::dsp::RdsDecoder rds_;
    cascade::dsp::DemodMode mode_ = cascade::dsp::DemodMode::WFM;
    bool keep_;
    float fmScale_ = 1.0f;
    std::size_t blocks_ = 0;
    Feeds feeds_;
    std::vector<std::complex<float>> chanBuf_;
    std::vector<float> audioBuf_, leftBuf_, rightBuf_, preAgc_, decoderBuf_, mpxBuf_, outL_, outR_;
};

}  // namespace demodchain
