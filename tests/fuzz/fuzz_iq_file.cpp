// Fuzz target: an I/Q recording opened through the file source (RIFF/WAVE).
//
// The header is walked chunk by chunk on the GUI thread when a user picks a
// file, and every field of it - sizes, offsets, format tag, channel count, bit
// depth, sample rate - is the file's own say-so. After a successful open the
// source is read the way the pipeline reads it.
//
// Properties: a source that opens reports a positive, finite sample rate and
// never hands back more frames than were asked for; every sample it delivers is
// finite (a float32 recording can encode NaN and infinity, and nothing
// downstream defends against them); a source that fails to open stays inert and
// says why.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fuzz_common.hpp"
#include "source/iq_file_source.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string path = fuzz::writeScratch("capture.wav", data, size);

    cascade::source::IqFileSource src;
    if (!src.open(path)) {
        FUZZ_REQUIRE(src.lastError() != nullptr && src.lastError()[0] != '\0');
        FUZZ_REQUIRE(!src.start());
        return 0;
    }

    const double rate = src.sampleRateHz();
    FUZZ_REQUIRE(rate > 0.0 && std::isfinite(rate));
    FUZZ_REQUIRE(src.start());

    // Block sizes picked from the input itself, so odd sizes and sizes that
    // straddle the loop point are reached.
    fuzz::Bytes in(data, size);
    std::vector<std::complex<float>> buf(4096);
    for (int i = 0; i < 6; ++i) {
        const std::size_t want = 1 + in.u32() % buf.size();
        const std::size_t got = src.read(buf.data(), want);
        FUZZ_REQUIRE(got <= want);
        for (std::size_t k = 0; k < got; ++k) {
            FUZZ_REQUIRE(std::isfinite(buf[k].real()) && std::isfinite(buf[k].imag()));
        }
    }
    src.stop();
    return 0;
}
