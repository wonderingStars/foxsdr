// test_aor_protocol.cpp - the AOR digital-I/Q word format and the aligner,
// proven against words built BY HAND from the AOR document's own formula.
//
// WHERE THE EXPECTATIONS COME FROM. There is no AOR receiver on this bench.
// Every expectation below is either a literal worked out by hand from the
// unpack formula in AOR's "Digital I/Q USB Interface Developer Information"
// Rev 1.1 (each one says how), or a word built by encodeWord() below, which
// is the formula run BACKWARDS and written independently of the driver's
// forward version in src/source/aor_protocol.hpp. Tested against a fake
// stream only; not tested on hardware.
//
// BOUNDS. Every buffer handed to the aligner is a std::vector of exactly the
// size passed, and every output buffer exactly the capacity passed, so an
// AddressSanitizer build of this file (-fsanitize=address) turns any read or
// write past either end into a failure. The report for this change carries
// that run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "source/aor_protocol.hpp"
#include "test_check.hpp"

namespace aor = cascade::source::aor;

namespace {

struct Sample {
    std::int32_t i;
    std::int32_t q;
};

// The formula backwards. ti = si * 4 must have its low three bits clear
// (bits 0-2 are never set by any term), so si is even. Bits 31-24 come from
// b0, 23-17 from b1 bits 7-1, 16-9 from b2, 8-3 from b3 bits 6-1. Bit 9 is
// reachable from both b2 bit 0 and b3 bit 7; this puts it in b2 and leaves
// b3 bit 7 clear. Marker bits: b1 bit 0 = 1, b3/b5/b7 bit 0 = 0.
void encodeHalf(std::int32_t s, bool first, std::uint8_t* b) {
    const std::uint32_t t = static_cast<std::uint32_t>(s) * 4u;
    b[0] = static_cast<std::uint8_t>(t >> 24);
    b[1] = static_cast<std::uint8_t>(((t >> 16) & 0xFEu) | (first ? 1u : 0u));
    b[2] = static_cast<std::uint8_t>((t >> 9) & 0xFFu);
    b[3] = static_cast<std::uint8_t>((t >> 2) & 0x7Eu);
}

void encodeWord(const Sample& s, std::uint8_t* w) {
    encodeHalf(s.i, true, w);
    encodeHalf(s.q, false, w + 4);
}

std::vector<Sample> randomSamples(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::int32_t> d(-(1 << 28), (1 << 28) - 1);
    std::vector<Sample> v(n);
    for (Sample& s : v) { s = Sample{d(rng) * 2, d(rng) * 2}; }
    return v;
}

std::vector<std::uint8_t> encodeStream(const std::vector<Sample>& v) {
    std::vector<std::uint8_t> bytes(v.size() * aor::kWordBytes);
    for (std::size_t k = 0; k < v.size(); ++k) { encodeWord(v[k], bytes.data() + k * 8); }
    return bytes;
}

std::complex<float> expected(const Sample& s) {
    return {static_cast<float>(s.i / 1073741824.0), static_cast<float>(s.q / 1073741824.0)};
}

// Pushes `bytes` through `a` in the chunks given, each chunk its own exactly
// sized vector and each output its own exactly sized vector.
std::vector<std::complex<float>> feed(aor::Aligner& a, const std::vector<std::uint8_t>& bytes,
                                      const std::vector<std::size_t>& chunks) {
    std::vector<std::complex<float>> all;
    std::size_t at = 0;
    for (std::size_t c : chunks) {
        if (at + c > bytes.size()) { c = bytes.size() - at; }
        std::vector<std::uint8_t> piece(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(at + c));
        std::vector<std::complex<float>> out(a.maxSamplesFor(piece.size()));
        const std::size_t n = a.push(piece.data(), piece.size(), out.data(), out.size());
        CHECK(n <= out.size());
        all.insert(all.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        at += c;
    }
    return all;
}

bool sameSamples(const std::vector<std::complex<float>>& got, const std::vector<Sample>& want,
                 std::size_t wantFrom = 0) {
    if (got.size() != want.size() - wantFrom) {
        std::printf("  size %zu, expected %zu\n", got.size(), want.size() - wantFrom);
        return false;
    }
    for (std::size_t k = 0; k < got.size(); ++k) {
        if (got[k] != expected(want[wantFrom + k])) {
            std::printf("  sample %zu differs\n", k);
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    // --- START / STOP, byte for byte from the AOR document ----------------
    {
        const std::uint8_t start[] = {0x5A, 0xA5, 0x00, 0x02, 0x41, 0x53};
        const std::uint8_t stop[] = {0x5A, 0xA5, 0x00, 0x02, 0x41, 0x45};
        for (std::size_t k = 0; k < 6; ++k) {
            CHECK(aor::kStartCommand[k] == start[k]);
            CHECK(aor::kStopCommand[k] == stop[k]);
        }
        CHECK(aor::kCommandEndpoint == 0x02);
        CHECK(aor::kIqEndpoint == 0x86);
        CHECK(aor::kSampleRateHz == 1125000.0);
    }

    // --- the unpack, literals worked by hand ------------------------------
    {
        // All zero data with the markers set: ti = 0.
        const std::uint8_t w0[8] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        CHECK(aor::unpackWord(w0).i == 0);
        CHECK(aor::unpackWord(w0).q == 0);
        // b0 = 0x40: ti = 0x40000000, si = 0x10000000 = 268435456, which is
        // exactly 0.25 at the 2^-30 scale. Q the same from b4 = 0xC0:
        // tq = 0xC0000000 = -1073741824, sq = -268435456, -0.25.
        const std::uint8_t w1[8] = {0x40, 0x01, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00};
        CHECK(aor::unpackWord(w1).i == 268435456);
        CHECK(aor::unpackWord(w1).q == -268435456);
        CHECK(aor::toComplex(aor::unpackWord(w1)) == std::complex<float>(0.25f, -0.25f));
        // All data bits set: ti = FF000000 | FE0000 | 1FE00 | 3F8 = FFFFFFF8,
        // (int32) -8, / 4 = -2.
        const std::uint8_t w2[8] = {0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE};
        CHECK(aor::packedHalf(0xFF, 0xFF, 0xFF, 0xFE) == 0xFFFFFFF8u);
        CHECK(aor::unpackWord(w2).i == -2);
        CHECK(aor::unpackWord(w2).q == -2);
        // THE OR, NOT A SUM: b2 = 0x01 gives bit 9 via (b2 << 9) and
        // b3 = 0x80 gives bit 9 via ((b3 & 0xFE) << 2). ORed, ti = 0x200 and
        // si = 128; summed it would be 0x400 and 256.
        CHECK(aor::packedHalf(0x00, 0x01, 0x01, 0x80) == 0x200u);
        CHECK(aor::unpackHalf(0x00, 0x01, 0x01, 0x80) == 128);
        // The marker bits themselves never reach the value: b1 bit 0 and b3
        // bit 0 set or clear give the same ti.
        CHECK(aor::packedHalf(0x12, 0x35, 0x56, 0x79) == aor::packedHalf(0x12, 0x34, 0x56, 0x78));
        // Largest and smallest: b0 = 0x7F, rest all ones -> 0x7FFFFFF8 / 4 =
        // 0x1FFFFFFE; b0 = 0x80, rest zero -> -2^31 / 4 = -2^29 = -0.5.
        CHECK(aor::unpackHalf(0x7F, 0xFF, 0xFF, 0xFE) == 0x1FFFFFFE);
        CHECK(aor::unpackHalf(0x80, 0x00, 0x00, 0x00) == -(1 << 29));
        CHECK(aor::toComplex(aor::RawSample{-(1 << 29), 0}).real() == -0.5f);
    }

    // --- round trip through the hand-built inverse ------------------------
    {
        const std::vector<Sample> v = randomSamples(20000, 1);
        int bad = 0;
        for (const Sample& s : v) {
            std::uint8_t w[8];
            encodeWord(s, w);
            if (!aor::markerValid(w)) { ++bad; }
            const aor::RawSample r = aor::unpackWord(w);
            if (r.i != s.i || r.q != s.q) { ++bad; }
        }
        CHECK(bad == 0);
        // The extremes of the representable range.
        for (const std::int32_t s : {-(1 << 29), (1 << 29) - 2, 0, 2, -2}) {
            std::uint8_t w[8];
            encodeWord(Sample{s, -s == (1 << 29) ? 0 : -s}, w);
            CHECK(aor::unpackWord(w).i == s);
        }
    }

    // --- marker validity: exactly one of the 16 marker patterns ------------
    {
        int validCount = 0;
        for (unsigned m = 0; m < 16; ++m) {
            // Data bits all ones so they cannot be mistaken for markers.
            std::uint8_t w[8] = {0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE};
            w[1] |= (m & 1u) ? 1 : 0;
            w[3] |= (m & 2u) ? 1 : 0;
            w[5] |= (m & 4u) ? 1 : 0;
            w[7] |= (m & 8u) ? 1 : 0;
            const bool v = aor::markerValid(w);
            if (v) { ++validCount; }
            CHECK(v == (m == 1u));
        }
        CHECK(validCount == 1);
        // Only bit 0 of bytes 1, 3, 5, 7 matters.
        const std::uint8_t a[8] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const std::uint8_t b[8] = {0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFE, 0xFF, 0xFE};
        CHECK(aor::markerValid(a));
        CHECK(aor::markerValid(b));
    }

    // --- alignment from every one of the eight byte offsets ---------------
    //
    // `off` junk bytes of 0x00 ahead of the stream. Even offsets are what the
    // AOR document's 2-byte search finds directly: every sample is recovered
    // and exactly `off` bytes are discarded. Odd offsets need the aligner's
    // one addition (the parity flip, see aor_protocol.hpp): the samples inside
    // the first kParityFlipBytes are lost, and every sample after the lock is
    // exact.
    for (std::size_t off = 0; off < 8; ++off) {
        const std::vector<Sample> v = randomSamples(4000, static_cast<unsigned>(100 + off));
        std::vector<std::uint8_t> bytes(off, 0x00);
        const std::vector<std::uint8_t> s = encodeStream(v);
        bytes.insert(bytes.end(), s.begin(), s.end());
        aor::Aligner a;
        const auto got = feed(a, bytes, {bytes.size()});
        CHECK(a.locked());
        CHECK(a.locks() == 1);
        CHECK(a.losses() == 0);
        if (off % 2 == 0) {
            CHECK(sameSamples(got, v));
            CHECK(a.discardedBytes() == off);
            CHECK(a.parityFlips() == 0);
        } else {
            CHECK(a.parityFlips() == 1);
            CHECK(got.size() > 3000);
            CHECK(sameSamples(got, v, v.size() - got.size()));
            CHECK(a.discardedBytes() + got.size() * 8 == bytes.size());
        }
        CHECK(a.carriedBytes() == 0);
        if (off == 0 || off == 7) { std::printf("offset %zu: %zu samples, %llu bytes discarded\n", off, got.size(), static_cast<unsigned long long>(a.discardedBytes())); }
    }

    // --- a sample split across buffer boundaries, at every split point ----
    {
        const std::vector<Sample> v = randomSamples(12, 7);
        const std::vector<std::uint8_t> bytes = encodeStream(v);
        int bad = 0;
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            aor::Aligner a;
            const auto got = feed(a, bytes, {split, bytes.size() - split});
            if (!sameSamples(got, v)) {
                std::printf("  split at %zu\n", split);
                ++bad;
            }
        }
        CHECK(bad == 0);
        // Three-way splits, every pair of cut points.
        int bad3 = 0;
        for (std::size_t c1 = 0; c1 <= bytes.size(); ++c1) {
            for (std::size_t c2 = c1; c2 <= bytes.size(); ++c2) {
                aor::Aligner a;
                const auto got = feed(a, bytes, {c1, c2 - c1, bytes.size() - c2});
                if (!sameSamples(got, v)) { ++bad3; }
            }
        }
        CHECK(bad3 == 0);
        // One byte at a time.
        aor::Aligner a;
        const auto got = feed(a, bytes, std::vector<std::size_t>(bytes.size(), 1));
        CHECK(sameSamples(got, v));
    }

    // --- re-acquiring after injected garbage ------------------------------
    {
        const std::vector<Sample> va = randomSamples(300, 11);
        const std::vector<Sample> vb = randomSamples(300, 12);
        std::vector<std::uint8_t> bytes = encodeStream(va);
        // Ten bytes of garbage: an even slip, found by the document's search.
        for (int k = 0; k < 10; ++k) { bytes.push_back(0x00); }
        const std::vector<std::uint8_t> b = encodeStream(vb);
        bytes.insert(bytes.end(), b.begin(), b.end());
        aor::Aligner a;
        const auto got = feed(a, bytes, {1000, 7, 3, bytes.size()});
        std::vector<Sample> both = va;
        both.insert(both.end(), vb.begin(), vb.end());
        CHECK(sameSamples(got, both));
        CHECK(a.losses() == 1);
        CHECK(a.locks() == 2);
        CHECK(a.discardedBytes() == 10);
    }
    {
        // Garbage that LOOKS like part of a word: a valid-marker word's first
        // three bytes (a short burst of a truncated sample), then the stream.
        // Five bytes in all - an odd slip - so recovery needs the parity flip
        // and the samples inside that window are given up; everything after
        // the re-lock is exact.
        const std::vector<Sample> va = randomSamples(200, 21);
        const std::vector<Sample> vb = randomSamples(2000, 22);
        std::vector<std::uint8_t> bytes = encodeStream(va);
        const std::uint8_t junk[5] = {0x12, 0x01, 0x34, 0x56, 0x02};
        bytes.insert(bytes.end(), junk, junk + 5);
        const std::vector<std::uint8_t> b = encodeStream(vb);
        bytes.insert(bytes.end(), b.begin(), b.end());
        aor::Aligner a;
        const auto got = feed(a, bytes, {bytes.size()});
        CHECK(a.losses() >= 1);
        CHECK(a.locked());
        CHECK(got.size() > va.size() + 1000);
        // The first 200 are va exactly; the tail is vb exactly.
        std::vector<std::complex<float>> head(got.begin(), got.begin() + 200);
        CHECK(sameSamples(head, va));
        const std::size_t tailN = 1000;
        std::vector<std::complex<float>> tail(got.end() - static_cast<std::ptrdiff_t>(tailN), got.end());
        CHECK(sameSamples(tail, vb, vb.size() - tailN));
    }

    // --- realistic mixed packet lengths: exactly the samples fed in -------
    {
        const std::vector<Sample> v = randomSamples(200000, 31);
        const std::vector<std::uint8_t> bytes = encodeStream(v);
        std::mt19937 rng(5);
        const std::size_t lengths[4] = {0, 512, 1024, 1536};
        std::vector<std::size_t> transfers;
        std::size_t total = 0;
        while (total < bytes.size()) {
            // One transfer = 128 packets of mixed actual lengths, concatenated.
            std::size_t t = 0;
            for (std::size_t p = 0; p < aor::kIsoPacketsPerTransfer; ++p) { t += lengths[rng() % 4]; }
            transfers.push_back(t);
            total += t;
        }
        aor::Aligner a;
        const auto got = feed(a, bytes, transfers);
        CHECK(sameSamples(got, v));
        CHECK(a.locks() == 1);
        CHECK(a.losses() == 0);
        CHECK(a.discardedBytes() == 0);
    }

    // --- an output buffer too small keeps the rest for next time ----------
    {
        const std::vector<Sample> v = randomSamples(100, 41);
        const std::vector<std::uint8_t> bytes = encodeStream(v);
        aor::Aligner a;
        std::vector<std::complex<float>> all;
        std::vector<std::complex<float>> out(7);
        std::size_t n = a.push(bytes.data(), bytes.size(), out.data(), out.size());
        CHECK(n == 7);
        all.insert(all.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        for (;;) {
            n = a.push(nullptr, 0, out.data(), out.size());
            CHECK(n <= out.size());
            if (n == 0) { break; }
            all.insert(all.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        }
        CHECK(sameSamples(all, v));
        // Zero capacity reads and writes nothing.
        aor::Aligner z;
        CHECK(z.push(bytes.data(), bytes.size(), nullptr, 0) == 0);
        CHECK(z.carriedBytes() == bytes.size());
    }

    // --- nothing emitted from noise ---------------------------------------
    {
        // Pure zeros never satisfy byte 1's marker, so nothing locks and the
        // carry never grows past one search window.
        std::vector<std::uint8_t> zeros(100000, 0x00);
        aor::Aligner a;
        const auto got = feed(a, zeros, {100000});
        CHECK(got.empty());
        CHECK(!a.locked());
        CHECK(a.carriedBytes() < aor::Aligner::kLockWords * aor::kWordBytes);
    }

    return testSummary("test_aor_protocol");
}
