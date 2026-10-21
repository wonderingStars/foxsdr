// Fuzz target: the receive chain behind a source, at a sample rate nobody planned.
//
// A source reports a rate, and from that rate the application builds its
// channel filter (Vfo), a demodulator, the stereo and RDS decoders and the
// noise reducer, each baking the rate into a filter design. "An unexpected
// rate" is how a plugin crashed in the field this month; this target puts the
// built-in chain through the same: any rate the pipeline would accept
// (8 kHz to 61.44 MHz, whole or not), any decimation that leaves a channel,
// any bandwidth and tuning offset a user can type, then blocks of I/Q of sizes
// the input chooses (including empty and prime ones).
//
// Output buffers are exactly the size the documented contract requires, each in
// its own heap block, so AddressSanitizer sees a step past either end.
//
// Properties: a stage never reports more samples than its buffer holds; the
// demodulator and the stereo and noise stages produce exactly one output per
// input where they promise it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dsp/demod.hpp"
#include "dsp/noise_reduction.hpp"
#include "dsp/rds.hpp"
#include "dsp/stereo_fm.hpp"
#include "dsp/vfo.hpp"
#include "fuzz_common.hpp"

using cascade::dsp::DemodMode;
using cascade::dsp::Demodulator;
using cascade::dsp::NoiseReduction;
using cascade::dsp::RdsDecoder;
using cascade::dsp::StereoFm;
using cascade::dsp::Vfo;

namespace {

constexpr double kMinRate = 8000.0;
constexpr double kMaxRate = 61.44e6;

// A rate the pipeline would accept: log-uniform over the whole range, or one of
// the rates real receivers report, with an optional fractional part (a
// non-integer rate is refused by the pipeline but a source can still report one).
double pickRate(fuzz::Bytes& in) {
    static const double kCommon[] = {8000,    44100,   48000,   96000,   192000,  250000,
                                     1024000, 1200000, 2000000, 2048000, 2400000, 2500000,
                                     3200000, 5000000, 10000000, 20000000, 61440000};
    const std::uint8_t sel = in.u8();
    double rate;
    if ((sel & 3) == 0) {
        rate = kCommon[in.u8() % (sizeof(kCommon) / sizeof(kCommon[0]))];
        rate += static_cast<double>(static_cast<std::int8_t>(in.u8()));  // 'just off' a common rate
    } else {
        const double u = static_cast<double>(in.u32()) / 4294967296.0;
        rate = kMinRate * std::pow(kMaxRate / kMinRate, u);
    }
    if ((sel & 4) != 0) { rate += static_cast<double>(in.u8()) / 256.0; }
    return std::clamp(rate, kMinRate, kMaxRate);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);

    const double inputRate = pickRate(in);
    // A decimation that leaves a channel of at least 8 kHz, as the pipeline's
    // own search guarantees.
    const unsigned maxDecim = static_cast<unsigned>(std::max(1.0, std::floor(inputRate / kMinRate)));
    const unsigned decim = in.inRange(1, std::min(maxDecim, 4096u));
    const double bandwidth = (in.u8() & 1) ? in.rawDouble() : static_cast<double>(in.u32() % 400000);
    const double offset = (in.u8() & 1) ? in.rawDouble() : static_cast<double>(in.u32() % 3000000) - 1.5e6;

    Vfo vfo(inputRate, decim, bandwidth);
    vfo.setOffsetHz(offset);
    const double chan = vfo.channelRateHz();
    FUZZ_REQUIRE(chan > 0.0 && std::isfinite(chan));

    Demodulator demod(chan);
    demod.setMode(static_cast<DemodMode>(in.u8() % cascade::dsp::kDemodModeCount));
    demod.setDeemphasisUs((in.u8() & 1) ? 50.0 : static_cast<double>(in.u8()));
    demod.setSsbBandwidthHz(static_cast<double>(in.u32() % 20000));

    StereoFm stereo(chan);
    stereo.setForceMono((in.u8() & 1) != 0);
    RdsDecoder rds(chan);
    NoiseReduction nr(48000.0, 512);
    nr.setEnabled(true);
    nr.setStrength(static_cast<float>(in.u8()) / 255.0f);

    // Occasionally change the bandwidth mid-stream: it redesigns the filter and
    // clears its history, and the buffers sized before must still be enough.
    const bool retune = (in.u8() & 7) == 0;
    const double bandwidth2 = static_cast<double>(in.u32() % 400000);

    // The I/Q stream: bytes of the input, as a slowly drifting tone plus the
    // bytes as noise, so filters see something other than silence.
    for (int block = 0; block < 6 && in.remaining() > 0; ++block) {
        const std::size_t n = in.u32() % 9000;
        std::vector<std::complex<float>> iq(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float re = (static_cast<float>(in.u8()) - 128.0f) / 128.0f;
            const float im = (static_cast<float>(in.u8()) - 128.0f) / 128.0f;
            iq[i] = {re, im};
        }
        if (retune && block == 2) { vfo.setBandwidthHz(bandwidth2); }

        const std::size_t cap = n / decim + 1;
        std::unique_ptr<std::complex<float>[]> ch(new std::complex<float>[cap]);
        const std::size_t m = vfo.process(iq.data(), n, ch.get(), cap);
        FUZZ_REQUIRE(m <= cap);

        std::unique_ptr<float[]> audio(new float[m == 0 ? 1 : m]);
        FUZZ_REQUIRE(demod.process(ch.get(), m, audio.get()) == m);

        std::unique_ptr<float[]> left(new float[m == 0 ? 1 : m]);
        std::unique_ptr<float[]> right(new float[m == 0 ? 1 : m]);
        FUZZ_REQUIRE(stereo.process(audio.get(), m, left.get(), right.get()) == m);
        FUZZ_REQUIRE(rds.process(audio.get(), m) == m);

        std::unique_ptr<float[]> cleaned(new float[m == 0 ? 1 : m]);
        FUZZ_REQUIRE(nr.process(audio.get(), m, cleaned.get()) == m);
    }
    return 0;
}
