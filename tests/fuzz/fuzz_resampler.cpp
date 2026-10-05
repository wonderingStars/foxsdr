// Fuzz target: the rational resampler, at ratios and block sizes nobody planned.
//
// RationalResampler sits behind the audio path (channel rate to 48 kHz) and in
// front of every plugin decoder that wants a rate other than the one its source
// runs at. The ratio comes from a device's reported sample rate or a plugin's
// declared one; the block size from whatever the source thread read. A field
// crash this month was an over-read in exactly this kind of code, at a rate no
// test had used.
//
// Every buffer handed in is EXACTLY the size the contract allows - an input of
// exactly nIn floats, an output of exactly outCap - in its own heap block, so
// AddressSanitizer sees any step past either end. The input is split into
// blocks of input-chosen sizes (including empty ones), and the blocked result is
// compared against the same stream in one call: the header promises they are
// bit-identical.
//
// Properties: never more than outCap outputs; never more than maxOut(n); the
// blocked and unblocked streams agree; an undersized outCap drops outputs
// without writing past it, as documented.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "dsp/resampler.hpp"
#include "fuzz_common.hpp"

using cascade::dsp::RationalResampler;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);

    // L and M: small enough that the filter (about L * taps floats) stays cheap,
    // wide enough to hit coprime pairs, L > M, L < M, L == M and the 1/1 path.
    const unsigned interp = in.inRange(1, 2400);
    const unsigned decim = in.inRange(1, 200000);
    const unsigned taps = in.inRange(1, 40);
    RationalResampler rs(interp, decim, taps);

    // The signal: the rest of the input as floats, bounded.
    const std::size_t total = std::min<std::size_t>(in.remaining() / 2, 12000);
    std::vector<float> signal(total);
    for (std::size_t i = 0; i < total; ++i) {
        signal[i] = (static_cast<float>(in.u8()) - 128.0f) / 128.0f;
    }

    // Reference: one call over the whole stream.
    RationalResampler whole(interp, decim, taps);
    std::vector<float> ref(whole.maxOut(total));
    const std::size_t refN = whole.process(signal.data(), total, ref.data(), ref.size());
    FUZZ_REQUIRE(refN <= whole.maxOut(total));

    // The same stream in input-chosen blocks, each through exact-size buffers.
    std::vector<float> got;
    std::size_t pos = 0;
    int emptyBlocks = 0;
    while (pos < total) {
        std::size_t n = std::min<std::size_t>(in.u32() % 3000, total - pos);
        // Empty blocks are part of the contract, but the stream must move on:
        // once the input's choices run out every draw is zero.
        if (n == 0 && ++emptyBlocks > 3) { n = std::min<std::size_t>(1, total - pos); }
        std::unique_ptr<float[]> block(new float[n == 0 ? 1 : n]);
        if (n != 0) { std::memcpy(block.get(), signal.data() + pos, n * sizeof(float)); }
        const std::size_t cap = rs.maxOut(n);
        std::unique_ptr<float[]> out(new float[cap == 0 ? 1 : cap]);
        const std::size_t produced = rs.process(block.get(), n, out.get(), cap);
        FUZZ_REQUIRE(produced <= cap);
        got.insert(got.end(), out.get(), out.get() + produced);
        pos += n;
    }
    FUZZ_REQUIRE(got.size() == refN);
    for (std::size_t i = 0; i < refN; ++i) {
        // Bit-identical per the header; compared as bit patterns so a NaN would
        // not hide a mismatch.
        FUZZ_REQUIRE(std::memcmp(&got[i], &ref[i], sizeof(float)) == 0);
    }

    // The documented drop-don't-overrun contract: an output buffer smaller than
    // maxOut(n) is never written past, and the stream phase still advances.
    // (A debug build asserts on it instead, so only where the contract is the
    // release one.)
#ifdef NDEBUG
    RationalResampler small(interp, decim, taps);
    const std::size_t cap = in.u8() % 8;
    std::unique_ptr<float[]> tiny(new float[cap == 0 ? 1 : cap]);
    std::size_t produced = 0;
    if (total != 0) { produced = small.process(signal.data(), total, tiny.get(), cap); }
    FUZZ_REQUIRE(produced <= cap);
#endif
    return 0;
}
