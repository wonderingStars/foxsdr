// fm_stereo_rds_source.hpp - an FM broadcast transmitter as an IqSource, for the
// tests that need a REAL stereo multiplex with a REAL RDS subcarrier behind the
// receiver: tests/test_pipeline_audio.cpp (the pipeline's P7 wiring) and
// tests/test_engine_api_rds_real.cpp (what the engine publishes of it, API 0.3 8.4).
//
// Moved out of test_pipeline_audio.cpp unchanged (engine stage 4, window port
// group 5) so both hold the one signal. The reference signal is built from the
// published broadcast-FM and RDS standards (subcarrier frequencies, the
// 0.45/0.09/0.45 composite proportions, the shortened cyclic (26,16) block code
// and its offset words), the same clean-room way tests/test_rds.cpp builds its
// own; nothing is compared against a constant taken from the implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "source/iq_source.hpp"

namespace fmrds {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPilotHz = 19000.0;
// Arbitrary non-zero pilot phase: a receiver that only works when the
// transmitter's pilot happens to start at phase 0 is not a receiver.
constexpr double kPilotPhase = 0.7;
constexpr double kSubcarrier38Hz = 38000.0;
constexpr double kRdsCarrierHz = 57000.0;
constexpr double kRdsHalfBitHz = 2375.0;  // 2 * 57000/48
constexpr double kAudioToneHz = 1000.0;

// Input rate for the whole test. 250 kHz gives decimation 1 and therefore a
// 250 kHz channel — the same composite rate the off-air --rds-check bench
// path uses — and leaves the 0.9 * channel-rate VFO filter wide enough for a
// Carson bandwidth of 2 * (50 + 57) = 214 kHz. At the app's usual 200 kHz
// channel the RDS sidebands would fold, which would be a defect of the TEST
// signal, not of the receiver.
constexpr double kInputRateHz = 250000.0;
constexpr double kPeakDeviationHz = 50000.0;
constexpr double kVfoBandwidthHz = 220000.0;

// --- RDS encoder (clean room, from the standard) -----------------------------

constexpr std::uint32_t kGen = 0x5B9u;  // x^10+x^8+x^7+x^5+x^4+x^3+1
constexpr std::uint16_t kOffA = 0x0FCu;
constexpr std::uint16_t kOffB = 0x198u;
constexpr std::uint16_t kOffC = 0x168u;
constexpr std::uint16_t kOffD = 0x1B4u;

inline std::uint16_t crc10(std::uint16_t data) {
    std::uint32_t reg = static_cast<std::uint32_t>(data) << 10;
    for (int bit = 25; bit >= 10; --bit) {
        if (((reg >> bit) & 1u) != 0u) { reg ^= (kGen << (bit - 10)); }
    }
    return static_cast<std::uint16_t>(reg & 0x3FFu);
}

struct BitStream {
    std::vector<std::uint8_t> bits;

    void push(std::uint32_t value, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((value >> i) & 1u));
        }
    }
    void block(std::uint16_t data, std::uint16_t offset) {
        push(data, 16);
        push(static_cast<std::uint32_t>(crc10(data) ^ offset), 10);
    }
    void group(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d) {
        block(a, kOffA);
        block(b, kOffB);
        block(c, kOffC);
        block(d, kOffD);
    }
};

inline std::uint16_t blockB(unsigned type, bool tp, unsigned pty, unsigned rest5) {
    return static_cast<std::uint16_t>(((type & 0xFu) << 12) | (0u << 11) |
                                      ((tp ? 1u : 0u) << 10) |
                                      ((pty & 0x1Fu) << 5) | (rest5 & 0x1Fu));
}

inline std::uint16_t pack2(char hi, char lo) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(hi)) << 8) |
        static_cast<std::uint8_t>(lo));
}

// Interleaved 0A (PS) and 2A (RadioText) groups, repeated: a real station
// sends both continuously, and interleaving them exercises the group-type
// dispatch the decoder does per group rather than one type in isolation.
inline std::vector<std::uint8_t> buildRdsBits(std::uint16_t pi, const std::string& ps8,
                                       const std::string& rt, unsigned pty,
                                       int cycles) {
    BitStream bs;
    for (int c = 0; c < cycles; ++c) {
        for (unsigned addr = 0; addr < 4; ++addr) {
            const unsigned rest = (1u << 4) | (1u << 3) | addr;  // TA, MS=music
            bs.group(pi, blockB(0u, true, pty, rest), 0xE0CDu,
                     pack2(ps8[2 * addr], ps8[2 * addr + 1]));
        }
        for (unsigned addr = 0; 4u * addr < rt.size(); ++addr) {
            bs.group(pi, blockB(2u, true, pty, addr & 0xFu),
                     pack2(rt[4 * addr], rt[4 * addr + 1]),
                     pack2(rt[4 * addr + 2], rt[4 * addr + 3]));
        }
    }
    return bs.bits;
}

// Differential encode (t[k] = t[k-1] XOR d[k]) then biphase: 1 -> (+1,-1),
// 0 -> (-1,+1).
inline std::vector<int> biphaseLevels(const std::vector<std::uint8_t>& dataBits) {
    std::vector<int> levels;
    levels.reserve(2 * dataBits.size());
    int prev = 0;
    for (const std::uint8_t b : dataBits) {
        prev ^= (b & 1);
        levels.push_back(prev ? +1 : -1);
        levels.push_back(prev ? -1 : +1);
    }
    return levels;
}

// --- An FM broadcast transmitter as an IqSource ------------------------------

// Generates a constant-envelope FM carrier at DC, modulated by a standard
// stereo multiplex carrying a HARD LEFT audio tone (L = tone, R = 0) plus the
// RDS subcarrier. Free-running, so the pipeline paces it in real time exactly
// as it paces the built-in generator; samples are synthesised on demand, so
// the whole transmission costs a few hundred bytes of state.
class FmStereoSource : public cascade::source::IqSource {
public:
    // `rateHz` is the rate the transmitter is synthesised at. Every leg but
    // one uses kInputRateHz; the rate-follow leg at the end runs it at a
    // radio's own rate, so the chain's decimation is exercised too.
    explicit FmStereoSource(std::vector<int> rdsLevels, double rateHz = kInputRateHz)
        : levels_(std::move(rdsLevels)), rate_(rateHz) {}

    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double) override { return false; }  // fixed-rate
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override {
        centerHz_ = hz;  // nominal: there is no tuner, the modulation is fixed
        return true;
    }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(sample_) / rate_;
            ++sample_;
            phase_ += kTwoPi * kPeakDeviationHz * mpx(t) / rate_;
            if (phase_ > kTwoPi) { phase_ -= kTwoPi; }
            if (phase_ < -kTwoPi) { phase_ += kTwoPi; }
            dst[i] = std::complex<float>(static_cast<float>(std::cos(phase_)),
                                         static_cast<float>(std::sin(phase_)));
        }
        return n;
    }

    const char* name() const override { return "FM stereo test transmitter"; }
    const char* lastError() const override { return ""; }

private:
    // The composite, normalised to unit peak so kPeakDeviationHz really is
    // the peak deviation. Proportions are the standard ones: 45% sum, 9%
    // pilot, 45% difference on a suppressed 38 kHz carrier, ~5% RDS.
    //
    // The subcarrier is cos(2p) where the pilot is cos(p) — the standard's
    // "exactly twice the pilot frequency AND twice its phase", which is the
    // whole reason a pilot is transmitted at all. Getting that phase relation
    // wrong does not merely attenuate the difference channel, it multiplies
    // it by cos(error): at 90 degrees the separation is exactly 0 dB, which
    // is indistinguishable from a receiver that never decoded stereo. The
    // pilot also carries a non-zero absolute phase, as a real transmitter's
    // does, so nothing here can accidentally pass on a phase of zero.
    double mpx(double t) const {
        const double left = std::sin(kTwoPi * kAudioToneHz * t);  // R = 0
        const double p = kTwoPi * kPilotHz * t + kPilotPhase;
        double v = 0.45 * left;  // (L+R)
        v += 0.09 * std::cos(p);
        v += 0.45 * left * std::cos(2.0 * p);  // (L-R), DSB-SC at 2p
        if (!levels_.empty()) {
            const std::size_t m =
                static_cast<std::size_t>(t * kRdsHalfBitHz) % levels_.size();
            v += 0.05 * static_cast<double>(levels_[m]) *
                 std::cos(kTwoPi * kRdsCarrierHz * t);
        }
        return v / 1.04;  // 0.45 + 0.09 + 0.45 + 0.05
    }

    std::vector<int> levels_;
    double rate_ = kInputRateHz;
    std::uint64_t sample_ = 0;
    double phase_ = 0.0;
    double centerHz_ = 100000000.0;
    bool running_ = false;
};

}  // namespace fmrds
