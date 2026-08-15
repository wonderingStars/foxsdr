// Tests for dsp/rds.hpp — the standalone RDS decoder.
//
// The reference is a complete RDS ENCODER built here in the test: it takes a
// PI code, a PS name, RadioText, PTY/TP/TA, assembles groups, computes each
// block's checkword, differentially encodes, biphase shapes and modulates the
// result onto a 57 kHz subcarrier summed into a full stereo composite. Nothing
// is compared against a constant read back from the implementation, and the
// two sides share no code: the encoder computes the checkword CRC by an
// explicit polynomial long division AND, as a cross-check, by a structurally
// different MSB-first shift register, and the two are proven equal over all
// 65536 information words before either is used.
//
// Clean room: the encoder was written from the published RDS/RBDS standard.
// No third-party RDS implementation was consulted.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/rds.hpp"

#include "test_check.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kSubcarrierHz = 57000.0;
constexpr double kHalfBitRateHz = 2375.0;  // 2 * 57000/48

using cascade::dsp::RdsDecoder;
using cascade::dsp::RdsState;

// --- Block code, from the published standard --------------------------------

// g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1
constexpr std::uint32_t kGen = 0x5B9u;
// Same polynomial with the x^10 term dropped, for the shift-register form.
constexpr std::uint16_t kGenLow = 0x1B9u;

// Offset words A, B, C, C', D as the standard tabulates them (d9..d0).
constexpr std::uint16_t kOffA = 0x0FCu;
constexpr std::uint16_t kOffB = 0x198u;
constexpr std::uint16_t kOffC = 0x168u;
constexpr std::uint16_t kOffCp = 0x350u;
constexpr std::uint16_t kOffD = 0x1B4u;

// Reference 1: the checkword is the remainder of x^10 * m(x) modulo g(x),
// computed by writing the long division out.
std::uint16_t crcByDivision(std::uint16_t data) {
    std::uint32_t reg = static_cast<std::uint32_t>(data) << 10;
    for (int bit = 25; bit >= 10; --bit) {
        if (((reg >> bit) & 1u) != 0u) { reg ^= (kGen << (bit - 10)); }
    }
    return static_cast<std::uint16_t>(reg & 0x3FFu);
}

// Reference 2: the same remainder from a 10-stage MSB-first LFSR. Different
// algorithm, different intermediate states; agreement over the whole input
// space is what makes either of them trustworthy as a reference.
std::uint16_t crcByLfsr(std::uint16_t data) {
    std::uint16_t reg = 0;
    for (int i = 15; i >= 0; --i) {
        const bool in = ((data >> i) & 1u) != 0u;
        const bool feedback = (((reg >> 9) & 1u) != 0u) != in;
        reg = static_cast<std::uint16_t>((reg << 1) & 0x3FFu);
        if (feedback) { reg = static_cast<std::uint16_t>(reg ^ kGenLow); }
    }
    return reg;
}

// --- Group assembly ----------------------------------------------------------

struct BitStream {
    std::vector<std::uint8_t> bits;

    void push(std::uint32_t value, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            bits.push_back(static_cast<std::uint8_t>((value >> i) & 1u));
        }
    }
    // 26-bit block: 16 information bits, then the 10-bit checkword formed by
    // adding the offset word to the CRC modulo 2.
    void block(std::uint16_t data, std::uint16_t offset) {
        push(data, 16);
        push(static_cast<std::uint32_t>(crcByDivision(data) ^ offset), 10);
    }
    void group(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d,
               bool versionB) {
        block(a, kOffA);
        block(b, kOffB);
        block(c, versionB ? kOffCp : kOffC);
        block(d, kOffD);
    }
};

// Block B layout: 4-bit group type, version bit (0 = A, 1 = B), TP, 5-bit PTY,
// then five group-type-specific bits.
std::uint16_t makeBlockB(unsigned type, bool versionB, bool tp, unsigned pty,
                         unsigned rest5) {
    return static_cast<std::uint16_t>(((type & 0xFu) << 12) |
                                      ((versionB ? 1u : 0u) << 11) |
                                      ((tp ? 1u : 0u) << 10) |
                                      ((pty & 0x1Fu) << 5) | (rest5 & 0x1Fu));
}

std::uint16_t pack2(char hi, char lo) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(hi)) << 8) |
        static_cast<std::uint8_t>(lo));
}

// Group 0A: PS name in two-character segments, plus TA / MS / DI and the
// alternative-frequency pair in block C.
void addPsGroups(BitStream& bs, std::uint16_t pi, const std::string& ps8,
                 unsigned pty, bool tp, bool ta) {
    for (unsigned addr = 0; addr < 4; ++addr) {
        const unsigned rest = ((ta ? 1u : 0u) << 4) | (1u << 3) /* MS = music */ |
                              (0u << 2) /* DI */ | addr;
        bs.group(pi, makeBlockB(0u, false, tp, pty, rest), 0xE0CDu,
                 pack2(ps8[2 * addr], ps8[2 * addr + 1]), false);
    }
}

// Group 0B: same PS payload, but the version bit is set and block C repeats
// the PI code instead of carrying alternative frequencies — which is what the
// C' offset word signals to the receiver.
void addPsGroupsB(BitStream& bs, std::uint16_t pi, const std::string& ps8,
                  unsigned pty, bool tp, bool ta) {
    for (unsigned addr = 0; addr < 4; ++addr) {
        const unsigned rest = ((ta ? 1u : 0u) << 4) | (1u << 3) | (0u << 2) | addr;
        bs.group(pi, makeBlockB(0u, true, tp, pty, rest), pi,
                 pack2(ps8[2 * addr], ps8[2 * addr + 1]), true);
    }
}

// RadioText is padded to a whole number of segments; the standard's way of
// ending a message short of the buffer length is a 0x0D carriage return.
std::string padRadioText(const std::string& rt, std::size_t chunk) {
    std::string s = rt;
    if (s.size() % chunk != 0) {
        s.push_back('\r');
        while (s.size() % chunk != 0) { s.push_back(' '); }
    }
    return s;
}

// Group 2A: four RadioText characters per group, two in block C and two in
// block D, at 4 * segment address.
void addRtGroups(BitStream& bs, std::uint16_t pi, const std::string& rt,
                 unsigned pty, bool tp, bool abFlag) {
    const std::string s = padRadioText(rt, 4);
    for (unsigned addr = 0; 4u * addr < s.size(); ++addr) {
        const unsigned rest = ((abFlag ? 1u : 0u) << 4) | (addr & 0xFu);
        bs.group(pi, makeBlockB(2u, false, tp, pty, rest),
                 pack2(s[4 * addr], s[4 * addr + 1]),
                 pack2(s[4 * addr + 2], s[4 * addr + 3]), false);
    }
}

// Group 2B: block C is the PI repeat, so only block D carries text — two
// characters per group, at 2 * segment address, capping the message at 32.
void addRtGroupsB(BitStream& bs, std::uint16_t pi, const std::string& rt,
                  unsigned pty, bool tp, bool abFlag) {
    const std::string s = padRadioText(rt, 2);
    for (unsigned addr = 0; 2u * addr < s.size() && addr < 16u; ++addr) {
        const unsigned rest = ((abFlag ? 1u : 0u) << 4) | (addr & 0xFu);
        bs.group(pi, makeBlockB(2u, true, tp, pty, rest), pi,
                 pack2(s[2 * addr], s[2 * addr + 1]), true);
    }
}

struct Payload {
    std::uint16_t pi = 0x1234;
    std::string ps = "CASCADE!";
    std::string rt;
    unsigned pty = 10;
    bool tp = true;
    bool ta = true;
};

std::vector<std::uint8_t> buildStream(const Payload& p, int cycles) {
    BitStream bs;
    for (int c = 0; c < cycles; ++c) {
        addPsGroups(bs, p.pi, p.ps, p.pty, p.tp, p.ta);
        if (!p.rt.empty()) {
            addRtGroups(bs, p.pi, p.rt, p.pty, p.tp, false);
        }
    }
    return bs.bits;
}

// --- Line coding and modulation ----------------------------------------------

// Differential encoding (t[k] = t[k-1] XOR d[k]) followed by biphase shaping:
// a 1 becomes the half-bit pair (+1, -1) and a 0 becomes (-1, +1).
//
// skipBits models tuning in part way through the transmission: the differential
// state is still built from the very first bit (the transmitter has been
// running all along), only the emitted waveform starts later.
std::vector<int> biphaseLevels(const std::vector<std::uint8_t>& dataBits,
                               std::size_t skipBits) {
    std::vector<int> levels;
    levels.reserve(2 * dataBits.size());
    int prev = 0;
    for (std::size_t k = 0; k < dataBits.size(); ++k) {
        prev ^= (dataBits[k] & 1);
        if (k < skipBits) { continue; }
        levels.push_back(prev ? +1 : -1);
        levels.push_back(prev ? -1 : +1);
    }
    return levels;
}

struct MpxOpts {
    double fs = 250000.0;
    double rdsAmp = 0.05;      // ~5% of the composite, as a real station sends
    double phase0 = 0.0;       // subcarrier phase the receiver must recover
    double carrierErrHz = 0.0; // receiver/transmitter subcarrier mismatch
    double halfRateHz = kHalfBitRateHz;  // lets a bit-clock error be injected
    bool stereo = true;        // mono audio + 19 kHz pilot + 38 kHz L-R
    std::size_t leadSamples = 0;
    // Uniform white noise added across the whole composite. The channel filter
    // keeps only +/-3.2 kHz of it around 57 kHz, and 517 taps of averaging make
    // what survives Gaussian, so a uniform generator is a fair stand-in for
    // receiver noise and stays exactly reproducible from the seed.
    double noiseAmp = 0.0;
    std::uint32_t noiseSeed = 0x5EED1234u;
};

// Sums the modulated RDS subcarrier into a broadcast composite. The stereo
// content is not decoration: the L-R subcarrier's upper sideband reaches
// 52 kHz here, 5 kHz from the RDS band at baseband and ten times its
// amplitude, so a decoder with a sloppy channel filter fails these tests.
std::vector<float> makeComposite(const std::vector<int>& levels,
                                 const MpxOpts& o) {
    const std::size_t dataSamples = static_cast<std::size_t>(
        std::ceil(static_cast<double>(levels.size()) / o.halfRateHz * o.fs));
    std::vector<float> mpx(o.leadSamples + dataSamples, 0.0f);
    std::uint32_t nseed = o.noiseSeed;
    for (std::size_t i = 0; i < mpx.size(); ++i) {
        const double t = static_cast<double>(i) / o.fs;
        double v = 0.0;
        if (o.noiseAmp > 0.0) {
            nseed = nseed * 1664525u + 1013904223u;
            v += o.noiseAmp *
                 (static_cast<double>(nseed >> 8) * (2.0 / 16777216.0) - 1.0);
        }
        if (o.stereo) {
            v += 0.45 * std::sin(kTwoPi * 1000.0 * t);   // L+R programme audio
            v += 0.09 * std::sin(kTwoPi * 19000.0 * t);  // stereo pilot
            v += 0.45 * std::sin(kTwoPi * 14000.0 * t) *
                 std::sin(kTwoPi * 38000.0 * t);  // L-R, sidebands to 52 kHz
        }
        if (i >= o.leadSamples) {
            const double td = static_cast<double>(i - o.leadSamples) / o.fs;
            const std::size_t m = static_cast<std::size_t>(td * o.halfRateHz);
            if (m < levels.size()) {
                v += o.rdsAmp * static_cast<double>(levels[m]) *
                     std::cos(kTwoPi * (kSubcarrierHz + o.carrierErrHz) * t +
                              o.phase0);
            }
        }
        mpx[i] = static_cast<float>(v);
    }
    return mpx;
}

std::vector<float> encodeToMpx(const Payload& p, int cycles, const MpxOpts& o,
                               std::size_t skipBits = 0) {
    return makeComposite(biphaseLevels(buildStream(p, cycles), skipBits), o);
}

RdsState runDecoder(RdsDecoder& d, const std::vector<float>& mpx) {
    CHECK(d.process(mpx.data(), mpx.size()) == mpx.size());
    return d.state();
}

// Fixed-seed LCG (Numerical Recipes constants) — no <random>, no device.
float lcgUnit(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

}  // namespace

int main() {
    // --- The two independent CRC references agree everywhere ----------------
    // Proves the arithmetic the encoder relies on, before any of it is used to
    // judge the decoder. 65536 words is the entire information-word space.
    {
        int mismatches = 0;
        for (std::uint32_t d = 0; d < 65536u; ++d) {
            if (crcByDivision(static_cast<std::uint16_t>(d)) !=
                crcByLfsr(static_cast<std::uint16_t>(d))) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
        // And the CRC is not a degenerate function: distinct words must not
        // all collapse to the same remainder, and the all-zero word must map
        // to zero (division of 0 by anything).
        CHECK(crcByDivision(0x0000u) == 0u);
        CHECK(crcByDivision(0x0001u) != crcByDivision(0x0002u));
        // The five offset words are distinct, or block positions could not be
        // told apart at all. static_assert rather than CHECK because these are
        // compile-time constants: a runtime CHECK on them is both a /W4
        // constant-conditional warning and a test that cannot ever run late.
        static_assert(kOffA != kOffB && kOffA != kOffC && kOffA != kOffCp &&
                          kOffA != kOffD,
                      "offset word A collides with another");
        static_assert(kOffB != kOffC && kOffB != kOffCp && kOffB != kOffD,
                      "offset word B collides with another");
        static_assert(kOffC != kOffCp && kOffC != kOffD && kOffCp != kOffD,
                      "offset words C/C'/D collide");
    }

    // --- Full decode: PI, PS, RadioText and flags, through a stereo MPX -----
    // Carrier phase offset and a 15 Hz subcarrier error exercise the carrier
    // loop; a 200 ppm bit-clock error exercises the timing loop's integrator.
    const Payload full{0x1234u, "CASCADE!", "cascade RDS test 1234", 10u, true,
                       true};
    MpxOpts mainOpts;
    mainOpts.phase0 = 2.2;  // well away from the loop's unstable +/-pi/2 points
    mainOpts.carrierErrHz = 15.0;
    mainOpts.halfRateHz = kHalfBitRateHz * 1.0002;
    const std::vector<float> mainMpx = encodeToMpx(full, 3, mainOpts);
    RdsState mainState;
    {
        RdsDecoder d(mainOpts.fs);
        mainState = runDecoder(d, mainMpx);
        CHECK(d.synced());
        CHECK(mainState.piValid);
        CHECK(mainState.pi == 0x1234u);
        CHECK(mainState.psValid);
        CHECK(mainState.ps == "CASCADE!");
        CHECK(mainState.radioText == "cascade RDS test 1234");
        CHECK(mainState.tp);
        CHECK(mainState.ta);
        CHECK(mainState.pty == 10u);
        CHECK(mainState.blockErrors == 0u);
        // 30 groups were transmitted; acquisition costs a few, and no group
        // can be counted twice.
        CHECK(mainState.groupsDecoded >= 24u);
        CHECK(mainState.groupsDecoded <= 30u);
    }

    // --- Carrier phase ambiguity: any subcarrier phase decodes identically ---
    // A squaring/Costas loop locks 180 degrees out half the time. Differential
    // decoding is what makes that harmless, and this is the test that proves
    // the decoder actually has it: phases either side of the two lock points
    // must all produce the same PI and PS.
    {
        const Payload psOnly{0x8FE2u, "RDS-TEST", "", 22u, false, false};
        for (const double phase : {0.0, 1.0, 2.2, 4.0}) {
            MpxOpts o;
            o.phase0 = phase;
            o.stereo = false;
            RdsDecoder d(o.fs);
            const RdsState st = runDecoder(d, encodeToMpx(psOnly, 3, o));
            CHECK(d.synced());
            CHECK(st.piValid);
            CHECK(st.pi == 0x8FE2u);
            CHECK(st.psValid);
            CHECK(st.ps == "RDS-TEST");
            CHECK(!st.tp);
            CHECK(!st.ta);
            CHECK(st.pty == 22u);
            CHECK(st.radioText.empty());  // no group 2 was ever sent
        }
    }

    // --- Inverted line code decodes identically ------------------------------
    // Every biphase half-bit negated. This is the convention question that
    // cannot be settled from the standard without the document in hand: does a
    // data 1 send (+,-) or (-,+)? The answer must not matter, because the
    // differential coding makes d[k] = t[k] XOR t[k-1] invariant under a global
    // inversion — the same property that absorbs the carrier loop's 180-degree
    // ambiguity. An encoder and decoder that shared an inverted convention
    // would pass every other test here and still fail on air, so this test
    // fixes the polarity question by making it irrelevant.
    {
        const Payload p{0x4A7Bu, "INVERTED", "inverted line code", 12u, true,
                        false};
        MpxOpts o;
        o.phase0 = 0.6;
        const std::vector<std::uint8_t> bits = buildStream(p, 3);

        std::vector<int> normal = biphaseLevels(bits, 0);
        std::vector<int> flipped = normal;
        for (int& lv : flipped) { lv = -lv; }

        RdsDecoder dn(o.fs);
        const RdsState sn = runDecoder(dn, makeComposite(normal, o));
        RdsDecoder di(o.fs);
        const RdsState si = runDecoder(di, makeComposite(flipped, o));

        CHECK(dn.synced());
        CHECK(di.synced());
        CHECK(sn.ps == "INVERTED");
        CHECK(si.ps == "INVERTED");      // the whole point
        CHECK(si.pi == 0x4A7Bu);
        CHECK(si.radioText == "inverted line code");
        CHECK(si.pty == 12u);
        CHECK(si.tp);
        CHECK(!si.ta);
        // Not merely "both work": inversion must be a no-op, so every field
        // and both counters have to match the un-inverted run exactly.
        CHECK(si.pi == sn.pi);
        CHECK(si.ps == sn.ps);
        CHECK(si.radioText == sn.radioText);
        CHECK(si.groupsDecoded == sn.groupsDecoded);
        CHECK(si.blockErrors == sn.blockErrors);
        CHECK(sn.blockErrors == 0u);
    }

    // --- The other biphase pairing ------------------------------------------
    // Delaying the transmission by ONE half-bit swaps which pairing of the
    // half-bit stream is the correct one, forcing the decoder's energy metric
    // to select the opposite alignment. Without that metric the bit boundary
    // would be wrong half the time on air.
    {
        const Payload p{0x11C7u, "HALFSHIF", "", 4u, false, false};
        MpxOpts o;
        o.phase0 = 1.4;
        o.stereo = false;
        std::vector<int> shifted = biphaseLevels(buildStream(p, 3), 0);
        shifted.insert(shifted.begin(), shifted.front());

        RdsDecoder d(o.fs);
        const RdsState st = runDecoder(d, makeComposite(shifted, o));
        CHECK(d.synced());
        CHECK(st.psValid);
        CHECK(st.ps == "HALFSHIF");
        CHECK(st.pi == 0x11C7u);
    }

    // --- Noisy channel: a sensitivity regression guard ------------------------
    // Off-air measurement (2026-08-15, BBC Radio Lancashire on 95.5 MHz and
    // Heart on 105.4 MHz) showed this decoder locking at block error rates of
    // 7.6% and 32.7%. This test pins comparable behaviour synthetically so a
    // future change that quietly costs several dB of sensitivity fails here
    // rather than only on a bench radio.
    {
        const Payload p{0x2B8Du, "NOISYCH ", "noisy", 6u, true, true};
        MpxOpts o;
        o.phase0 = 2.0;
        // 0.25 was chosen by measuring the cliff rather than guessing: across
        // three noise seeds this decoder is reliable to 0.28 and starts failing
        // at 0.32, so 0.25 sits about 2 dB inside the edge. Three seeds, not
        // one, so the result cannot hinge on a single lucky noise realisation.
        std::uint32_t totalErrors = 0;
        for (std::uint32_t seed : {0x5EED1234u, 0x0BADF00Du, 0x13579BDFu}) {
            MpxOpts o2 = o;
            o2.noiseAmp = 0.25;
            o2.noiseSeed = seed;
            RdsDecoder d(o2.fs);
            const RdsState st = runDecoder(d, encodeToMpx(p, 4, o2));
            CHECK(d.synced());
            CHECK(st.piValid);
            CHECK(st.pi == 0x2B8Du);
            CHECK(st.psValid);
            // Noise must never produce WRONG characters — a block reaches the
            // group decoder only after its checkword passes — so the recovered
            // text is exact or absent, never corrupted.
            CHECK(st.ps == "NOISYCH ");
            CHECK(st.radioText == "noisy");
            CHECK(st.groupsDecoded > 0u);
            totalErrors += st.blockErrors;
        }
        // The noise is genuinely reaching the decoder: if it were not (a broken
        // generator, a filter that removed it), every block would pass and this
        // would be a test of nothing.
        CHECK(totalErrors > 0u);

        // And it does not hallucinate a station out of overwhelming noise.
        {
            MpxOpts o3 = o;
            o3.noiseAmp = 3.0;
            RdsDecoder d(o3.fs);
            const RdsState st = runDecoder(d, encodeToMpx(p, 4, o3));
            CHECK(!d.synced());
            CHECK(!st.psValid);
            CHECK(st.ps.empty());
        }
    }

    // --- A different composite rate: 171 kHz, 9:1 decimation ----------------
    {
        const Payload p{0x2C41u, "RATE171 ", "", 3u, true, false};
        MpxOpts o;
        o.fs = 171000.0;
        o.phase0 = 0.7;
        RdsDecoder d(o.fs);
        const RdsState st = runDecoder(d, encodeToMpx(p, 3, o));
        CHECK(d.synced());
        CHECK(st.pi == 0x2C41u);
        CHECK(st.psValid);
        CHECK(st.ps == "RATE171 ");
        CHECK(st.tp);
        CHECK(!st.ta);
        CHECK(st.pty == 3u);
    }

    // --- Version-B groups: 0B/2B, the C' offset word, PI out of block C ------
    // Every block A here is deliberately corrupted, so the only surviving copy
    // of the PI code is the repeat a version-B group carries in block C. That
    // proves three things at once: C' is accepted where C is expected, the
    // version bit routes block C to PI instead of to data, and a station whose
    // block A keeps getting hit still identifies itself.
    {
        const std::uint16_t piB = 0x6D19u;
        const std::string rtB = "SHORT B-VERSION TEXT";  // 20 chars; 2B caps at 32
        auto buildB = [&](BitStream& out) {
            for (int c = 0; c < 2; ++c) {
                addPsGroupsB(out, piB, "B-GROUPS", 7u, true, false);
                addRtGroupsB(out, piB, rtB, 7u, true, false);
            }
        };
        MpxOpts o;
        o.fs = 171000.0;
        o.phase0 = 2.9;

        // Reference run: identical stream, block A intact. It pins how many
        // groups this transmission is worth once acquisition and the front
        // end's own latency have taken their cut, so the corrupted run below
        // can be compared against a measurement rather than a guess.
        BitStream ref;
        buildB(ref);
        RdsDecoder refDec(o.fs);
        const RdsState refState =
            runDecoder(refDec, makeComposite(biphaseLevels(ref.bits, 0), o));
        CHECK(refDec.synced());
        CHECK(refState.blockErrors == 0u);
        CHECK(refState.pi == piB);
        CHECK(refState.groupsDecoded >= 20u);

        BitStream bs;
        buildB(bs);
        const std::size_t groups = bs.bits.size() / 104u;
        CHECK(groups == 2u * (4u + 10u));
        for (std::size_t g = 0; g < groups; ++g) { bs.bits[g * 104u + 3u] ^= 1u; }

        RdsDecoder d(o.fs);
        const RdsState st =
            runDecoder(d, makeComposite(biphaseLevels(bs.bits, 0), o));
        CHECK(d.synced());
        CHECK(st.piValid);
        CHECK(st.pi == piB);  // block A never passed, so this came from block C
        CHECK(st.psValid);
        CHECK(st.ps == "B-GROUPS");
        CHECK(st.radioText == rtB);
        CHECK(st.tp);
        CHECK(st.pty == 7u);
        // Breaking block A of every group costs exactly those blocks: the same
        // number of groups is still interpreted as in the clean reference.
        CHECK(st.groupsDecoded == refState.groupsDecoded);
        // One counted error per group, give or take the ragged ends: sync is
        // acquired part way into a group whose block A was never examined, and
        // the front end's group delay means the last group's block A is counted
        // before its block D ever emerges.
        CHECK(st.blockErrors + 1u >= st.groupsDecoded);
        CHECK(st.blockErrors <= st.groupsDecoded + 2u);
    }

    // --- Sync acquisition from a random start: partial first group ----------
    // 47 bits in is the middle of block B of the first group, so the decoder
    // sees a fragment of a block before any real boundary arrives.
    {
        MpxOpts o;
        o.phase0 = 1.1;
        RdsDecoder d(o.fs);
        const RdsState st = runDecoder(d, encodeToMpx(full, 2, o, 47));
        CHECK(d.synced());
        CHECK(st.pi == 0x1234u);
        CHECK(st.psValid);
        CHECK(st.ps == "CASCADE!");
        CHECK(st.radioText == "cascade RDS test 1234");
        // Starting mid-block cannot manufacture checkword failures: the
        // decoder only counts blocks once it is locked.
        CHECK(st.blockErrors == 0u);
    }

    // --- A corrupted block is counted, and damages nothing already decoded ---
    {
        // Flip one information bit of block C in group 25, i.e. in the last of
        // the three cycles, long after PS and RadioText are complete. A single
        // bit error is always detected by a degree-10 generator, so the
        // syndrome cannot match offset C.
        std::vector<std::uint8_t> bits = buildStream(full, 3);
        const std::size_t badBit = 25u * 104u + 2u * 26u + 5u;
        CHECK(badBit < bits.size());
        bits[badBit] ^= 1u;

        RdsDecoder d(mainOpts.fs);
        const std::vector<float> mpx =
            makeComposite(biphaseLevels(bits, 0), mainOpts);
        const RdsState st = runDecoder(d, mpx);

        CHECK(d.synced());               // one bad block must not drop sync
        CHECK(st.blockErrors == 1u);     // exactly the block we broke
        CHECK(st.pi == 0x1234u);
        CHECK(st.psValid);
        CHECK(st.ps == "CASCADE!");      // untouched by the corrupt block
        CHECK(st.radioText == "cascade RDS test 1234");
        // Block B of that group still passed, so the group was still
        // interpreted: the corruption costs its own block, nothing more.
        CHECK(st.groupsDecoded == mainState.groupsDecoded);
    }

    // --- RadioText A/B flag toggle clears the buffer -------------------------
    // The new message is shorter than the old one and, being a whole number of
    // segments, carries no 0x0D terminator. Only an actual buffer clear can
    // stop the tail of the previous message showing through.
    {
        const std::string rt1 = "FIRST MESSAGE 0123456789ABCDEF";  // 30 chars
        const std::string rt2 = "SECOND MESSAGE!!";                // 16 chars
        CHECK(rt1.size() == 30u);
        CHECK(rt2.size() % 4u == 0u);

        BitStream bs;
        for (int c = 0; c < 2; ++c) {
            addPsGroups(bs, 0x51AAu, "TOGGLE  ", 5u, true, false);
            addRtGroups(bs, 0x51AAu, rt1, 5u, true, false);
        }
        for (int c = 0; c < 2; ++c) {
            addPsGroups(bs, 0x51AAu, "TOGGLE  ", 5u, true, false);
            addRtGroups(bs, 0x51AAu, rt2, 5u, true, true);
        }

        MpxOpts o;
        o.fs = 171000.0;
        o.phase0 = 3.0;
        RdsDecoder d(o.fs);
        const RdsState st =
            runDecoder(d, makeComposite(biphaseLevels(bs.bits, 0), o));
        CHECK(d.synced());
        CHECK(st.radioText == rt2);
        CHECK(st.radioText.size() == 16u);  // no residue from the 30-char text
        CHECK(st.ps == "TOGGLE  ");
    }

    // --- Chunked feeding equals one shot ------------------------------------
    {
        const Payload p{0x7A03u, "CHUNKED ", "chunk", 1u, false, true};
        MpxOpts o;
        o.phase0 = 0.4;
        const std::vector<float> mpx = encodeToMpx(p, 3, o);

        RdsDecoder one(o.fs);
        const RdsState a = runDecoder(one, mpx);

        RdsDecoder many(o.fs);
        // Ragged splits, including single-sample blocks, so every internal
        // stage sees a boundary in an awkward place.
        const std::size_t splits[] = {1u, 3u, 1023u, 2048u, 2049u, 5000u, 7u};
        std::size_t pos = 0;
        std::size_t s = 0;
        while (pos < mpx.size()) {
            std::size_t len = splits[s % (sizeof(splits) / sizeof(splits[0]))];
            ++s;
            if (len > mpx.size() - pos) { len = mpx.size() - pos; }
            CHECK(many.process(mpx.data() + pos, len) == len);
            pos += len;
        }
        const RdsState b = many.state();

        CHECK(one.synced() == many.synced());
        CHECK(a.pi == b.pi);
        CHECK(a.piValid == b.piValid);
        CHECK(a.psValid == b.psValid);
        CHECK(a.ps == b.ps);
        CHECK(a.radioText == b.radioText);
        CHECK(a.tp == b.tp);
        CHECK(a.ta == b.ta);
        CHECK(a.pty == b.pty);
        CHECK(a.groupsDecoded == b.groupsDecoded);
        CHECK(a.blockErrors == b.blockErrors);
        // ...and it actually decoded something, or the comparison is vacuous.
        CHECK(a.psValid);
        CHECK(a.ps == "CHUNKED ");
        CHECK(a.radioText == "chunk");
    }

    // --- reset() equals a fresh object ---------------------------------------
    {
        const Payload dirty{0x0AA0u, "OLDNAME ", "old text here", 31u, true,
                            true};
        const Payload clean{0x7A03u, "CHUNKED ", "chunk", 1u, false, true};
        MpxOpts o;
        o.phase0 = 0.4;
        const std::vector<float> dirtyMpx = encodeToMpx(dirty, 2, o);
        const std::vector<float> cleanMpx = encodeToMpx(clean, 3, o);

        RdsDecoder reused(o.fs);
        runDecoder(reused, dirtyMpx);
        CHECK(reused.state().pi == 0x0AA0u);  // it really was dirtied
        reused.reset();
        // Immediately after reset the object must look brand new.
        CHECK(!reused.synced());
        const RdsState blank = reused.state();
        CHECK(!blank.piValid && blank.pi == 0u);
        CHECK(!blank.psValid && blank.ps.empty());
        CHECK(blank.radioText.empty());
        CHECK(blank.groupsDecoded == 0u && blank.blockErrors == 0u);
        CHECK(!blank.tp && !blank.ta && blank.pty == 0u);

        const RdsState after = runDecoder(reused, cleanMpx);
        RdsDecoder fresh(o.fs);
        const RdsState freshState = runDecoder(fresh, cleanMpx);

        CHECK(reused.synced() == fresh.synced());
        CHECK(after.pi == freshState.pi);
        CHECK(after.ps == freshState.ps);
        CHECK(after.radioText == freshState.radioText);
        CHECK(after.tp == freshState.tp);
        CHECK(after.ta == freshState.ta);
        CHECK(after.pty == freshState.pty);
        CHECK(after.groupsDecoded == freshState.groupsDecoded);
        CHECK(after.blockErrors == freshState.blockErrors);
        CHECK(after.psValid && after.ps == "CHUNKED ");
    }

    // --- No signal: noise only ------------------------------------------------
    {
        std::uint32_t seed = 0xBEEF0117u;
        std::vector<float> noise(400000);
        for (auto& v : noise) { v = 0.3f * lcgUnit(seed); }

        RdsDecoder d(250000.0);
        const RdsState st = runDecoder(d, noise);
        CHECK(!d.synced());
        CHECK(!st.piValid);
        CHECK(!st.psValid);
        CHECK(st.ps.empty());       // no garbage name for a station that is not there
        CHECK(st.radioText.empty());
        CHECK(st.groupsDecoded == 0u);

        // Zero-length and null calls are legal no-ops.
        CHECK(d.process(noise.data(), 0) == 0u);
        CHECK(d.process(nullptr, 16) == 0u);
    }

    // --- A silent input is equally harmless -----------------------------------
    {
        const std::vector<float> silence(50000, 0.0f);
        RdsDecoder d(250000.0);
        const RdsState st = runDecoder(d, silence);
        CHECK(!d.synced());
        CHECK(!st.piValid);
        CHECK(st.ps.empty());
        CHECK(st.radioText.empty());
    }

    return testSummary("test_rds");
}
