// Fuzz target: the patch page's rate arithmetic, and the resampler it builds.
//
// When a plugin decoder is wired to a radio, the patch runner turns two rates
// - what the source runs at and what the plugin asked for - into an integer
// resampler ratio (resampleInputRate), picks a channel decimation for the
// radio (chooseChannelRate, wholeDecimation) and builds a RationalResampler
// from the result. The inputs are a device's reported rate and a THIRD PARTY'S
// declared one, so this is the "unexpected rate" path in its purest form: no
// DSP block runs until the arithmetic has already decided what to build.
//
// Properties: a ratio resampleInputRate returns has an interpolation factor
// within kMaxDecoderInterp (the bound that keeps the filter small) and is a
// positive whole rate near the one asked for; a decimation chooseChannelRate
// returns lands in the channel band; and the resampler built from them runs
// over a block into exactly-sized buffers without a fault.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

#include "core/patch_plan.hpp"
#include "dsp/resampler.hpp"
#include "fuzz_common.hpp"

using namespace cascade::core::patch;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);

    // Two rates, as the plan sees them. Raw doubles half the time (NaN, infinity,
    // negatives, denormals - what a hostile plugin could declare), otherwise a
    // plausible rate in the range a receiver or a decoder uses.
    const auto rate = [&in]() {
        if ((in.u8() & 1) != 0) { return in.rawDouble(); }
        return 1.0 + static_cast<double>(in.u32() % 80000000) + static_cast<double>(in.u8()) / 256.0;
    };
    const double inHz = rate();
    const double outHz = rate();

    // The search loop in resampleInputRate is bounded by 500 ppm of the input
    // rate, so its cost is linear in the rate. Callers hand it rates a receiver
    // can run at; the target keeps to that domain (the raw doubles above still
    // reach the early-outs, and everything up to the largest real rate).
    const bool tractable = std::isfinite(inHz) && inHz <= 2.0e9 && std::isfinite(outHz) &&
                           outHz <= 2.0e9;

    if (tractable) {
        const unsigned inR = resampleInputRate(inHz, outHz);
        if (inR != 0) {
            const auto out = static_cast<unsigned long long>(outHz + 0.5);
            const unsigned long long g = gcdU(out, inR);
            FUZZ_REQUIRE(out / g <= kMaxDecoderInterp);
            FUZZ_REQUIRE(std::fabs(static_cast<double>(inR) - inHz) <=
                         inHz * kMaxRateNudgePpm * 1e-6 + 1.0);

            // What buildDecoders does next.
            const auto outR = static_cast<unsigned>(outHz + 0.5);
            cascade::dsp::RationalResampler rs(outR, inR);
            const std::size_t n = in.u32() % 5000;
            std::vector<float> block(n);
            for (float& f : block) { f = (static_cast<float>(in.u8()) - 128.0f) / 128.0f; }
            const std::size_t cap = rs.maxOut(n);
            std::unique_ptr<float[]> outBuf(new float[cap == 0 ? 1 : cap]);
            FUZZ_REQUIRE(rs.process(block.data(), n, outBuf.get(), cap) <= cap);
        }
    }

    const RateChoice rc = chooseChannelRate(inHz);
    if (rc.ok) {
        FUZZ_REQUIRE(rc.decimation >= 1);
        FUZZ_REQUIRE(rc.rateHz >= kMinChannelRateHz && rc.rateHz <= kMaxChannelRateHz);
    }
    (void)wholeDecimation(inHz, outHz);
    return 0;
}
