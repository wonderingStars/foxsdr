// Bit-view library: CRC engine, catalogue, finder, additive/XOR/Fletcher checksums (crc.hpp).
//
// The finder is proven three ways: against the catalogue (ten known definitions from 3 to 5 frames),
// against definitions that are NOT in the catalogue (random width/polynomial/init/xor/reflection,
// the polynomial derived algebraically and the init/xor checked on held-out frames), and against
// frames with no CRC at all (it must say nothing). Ambiguity is asserted too: one frame, two
// frames and one-length frames each get the honest answer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/bits/crc.hpp"

#include <cstdio>
#include <cstring>

#include "bits_test_util.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {

std::vector<uint8_t> asciiDigits() { return {'1', '2', '3', '4', '5', '6', '7', '8', '9'}; }

// data + CRC (in `order`) + `tail` extra bytes + `head` extra bytes in front
std::vector<uint8_t> makeFrame(const CrcModel& m, const std::vector<uint8_t>& data, ByteOrder order, const std::vector<uint8_t>& head = {},
                               const std::vector<uint8_t>& tail = {}) {
    std::vector<uint8_t> f(head);
    f.insert(f.end(), data.begin(), data.end());
    const uint64_t c = crcBytes(m, data);
    const unsigned nb = m.width / 8;
    for (unsigned i = 0; i < nb; ++i) {
        const unsigned shift = 8 * (order == ByteOrder::Big ? nb - 1 - i : i);
        f.push_back(static_cast<uint8_t>(c >> shift));
    }
    f.insert(f.end(), tail.begin(), tail.end());
    return f;
}

bool reproduces(const CrcModel& m, const std::vector<uint8_t>& frame, ByteOrder order, size_t head, size_t tail) {
    const unsigned nb = m.width / 8;
    if (frame.size() < head + nb + tail + 1) return false;
    const size_t cpos = frame.size() - tail - nb;
    const std::vector<uint8_t> data(frame.begin() + static_cast<std::ptrdiff_t>(head), frame.begin() + static_cast<std::ptrdiff_t>(cpos));
    uint64_t v = 0;
    for (unsigned i = 0; i < nb; ++i) v = (v << 8) | frame[cpos + (order == ByteOrder::Big ? i : nb - 1 - i)];
    return crcBytes(m, data) == v;
}

const CatalogueEntry* byName(const char* sub) {
    for (const auto& e : crcCatalogue())
        if (std::strstr(e.model.name, sub)) return &e;
    return nullptr;
}

}  // namespace

int main() {
    Rng rng(0xC0DE);

    // ---- the engine against the catalogue's printed check values ----------------------------------
    for (const auto& e : crcCatalogue()) {
        const uint64_t got = crcBytes(e.model, asciiDigits());
        if (got != e.check) std::printf("  %s: got %llx want %llx\n", e.model.name, static_cast<unsigned long long>(got), static_cast<unsigned long long>(e.check));
        CHECK(got == e.check);
    }
    CHECK(crcCatalogue().size() == 18);
    // refin on a message that is not a whole number of bytes is refused, not guessed
    {
        bool ok = true;
        crcCompute(byName("MODBUS")->model, fromString("101"), &ok);
        CHECK(!ok);
        crcCompute(byName("XMODEM")->model, fromString("101"), &ok);
        CHECK(ok);
        CrcModel bad;
        crcCompute(bad, fromString("1"), &ok);
        CHECK(!ok);
        CHECK(reflectBits(0x01, 8) == 0x80 && reflectBits(0x8005, 16) == 0xA001);
        // a CRC of width 5 (non-byte) over bits: USB token CRC-5 (poly 0x05, init 0x1f, xorout 0x1f, reflected): self-consistency only
        CrcModel c5{5, 0x05, 0x1f, 0x1f, false, false, "t"};
        const Bits msg = fromString("10101001110");
        const uint64_t v = crcCompute(c5, msg);
        Bits full = msg;
        for (int j = 4; j >= 0; --j) full.push_back(static_cast<uint8_t>((v >> j) & 1u));
        // the register after message + CRC is the residue constant for any message
        CrcModel raw = c5;
        const uint64_t r1 = crcRegister(5, 0x05, 0x1f, full);
        const Bits msg2 = fromString("0110110001001");
        Bits full2 = msg2;
        const uint64_t v2 = crcCompute(c5, msg2);
        for (int j = 4; j >= 0; --j) full2.push_back(static_cast<uint8_t>((v2 >> j) & 1u));
        CHECK(r1 == crcRegister(5, 0x05, 0x1f, full2));
        (void)raw;
    }

    // ---- ten known definitions recovered from 3 to 5 sample frames ---------------------------------
    {
        const char* names[] = {"SMBUS", "MAXIM-DOW", "ROHC", "ARC", "MODBUS", "KERMIT", "IBM-3740", "X-25", "ISO-HDLC", "BZIP2", "OPENPGP"};
        int found = 0, total = 0;
        for (const char* n : names) {
            const CatalogueEntry* e = byName(n);
            CHECK(e != nullptr);
            if (!e) continue;
            for (int nfr = 3; nfr <= 5; ++nfr) {
                for (ByteOrder order : {ByteOrder::Big, ByteOrder::Little}) {
                    const size_t len = 6 + rng.below(14);
                    std::vector<std::vector<uint8_t>> frames;
                    for (int i = 0; i < nfr; ++i) frames.push_back(makeFrame(e->model, bitstest::randomBytes(rng, len), order));
                    const auto hits = findCrc(frames);
                    bool good = false;
                    for (const auto& h : hits)
                        if (h.catalogueMatch && h.model.width == e->model.width && h.model.poly == e->model.poly && h.model.init == e->model.init &&
                            h.model.xorout == e->model.xorout && h.model.refin == e->model.refin && h.model.refout == e->model.refout && h.order == order &&
                            h.headBytes == 0 && h.tailBytes == 0)
                            good = true;
                    ++total;
                    found += good ? 1 : 0;
                    if (!good) std::printf("  not found: %s, %d frames, %s order\n", n, nfr, order == ByteOrder::Big ? "big" : "little");
                    // every reported hit really reproduces every frame (no false claim)
                    for (const auto& h : hits)
                        for (const auto& f : frames) CHECK(reproduces(h.model, f, h.order, h.headBytes, h.tailBytes));
                }
            }
        }
        std::printf("  known CRC definitions recovered: %d/%d\n", found, total);
        CHECK(found == total);
    }

    // ---- head and tail bytes the CRC does not cover ---------------------------------------------------
    {
        const CrcModel m = byName("KERMIT")->model;
        std::vector<std::vector<uint8_t>> frames;
        for (int i = 0; i < 4; ++i) frames.push_back(makeFrame(m, bitstest::randomBytes(rng, 12), ByteOrder::Little, {0xAA, 0x55}, {0x0D}));
        const auto hits = findCrc(frames);
        bool good = false;
        for (const auto& h : hits) good |= h.catalogueMatch && h.model.poly == 0x1021 && h.headBytes == 2 && h.tailBytes == 1 && h.order == ByteOrder::Little;
        CHECK(good);
    }

    // ---- ONE frame: catalogue only, and it says a coincidence is possible --------------------------------
    {
        const CrcModel m = byName("ARC")->model;
        const auto hits = findCrc({makeFrame(m, bitstest::randomBytes(rng, 10), ByteOrder::Little)});
        bool good = false;
        for (const auto& h : hits)
            if (h.catalogueMatch && std::strstr(h.model.name, "ARC")) {
                good = true;
                CHECK(h.note.find("one frame only") != std::string::npos);
                CHECK(h.coincidence > 0.0 && h.coincidence < 1e-3);
            }
        CHECK(good);
        // nothing derivable from one frame: a definition that is NOT in the catalogue is not claimed
        const CrcModel odd{16, 0x0589, 0x1234, 0x00ff, false, true, ""};
        const auto none = findCrc({makeFrame(odd, bitstest::randomBytes(rng, 10), ByteOrder::Big)});
        for (const auto& h : none) CHECK(!(h.derived && h.model.poly == 0x0589 && h.candidates == 1));
        CHECK(findCrc({}).empty());
        CHECK(findCrc({{}, {}}).empty());
        CHECK(findCrc({{1, 2}}).empty());  // too short for head + data + check
    }

    // ---- definitions that are NOT in the catalogue: derived algebraically ---------------------------------
    {
        int tried = 0, polyOk = 0, heldOk = 0, uniqueClaims = 0, uniqueRight = 0;
        for (unsigned w : {8u, 16u, 32u})
            for (int t = 0; t < 12; ++t) {
                CrcModel m;
                m.width = w;
                m.poly = (rng.next() & crcMask(w)) | 1u;
                if (w < 64) m.poly &= crcMask(w);
                m.init = rng.next() & crcMask(w);
                m.xorout = rng.next() & crcMask(w);
                m.refin = rng.below(2) != 0;
                m.refout = rng.below(2) != 0;
                const size_t len = 8 + rng.below(10);
                const ByteOrder order = rng.below(2) ? ByteOrder::Big : ByteOrder::Little;
                std::vector<std::vector<uint8_t>> frames;
                for (int i = 0; i < 5; ++i) frames.push_back(makeFrame(m, bitstest::randomBytes(rng, len), order));
                const auto heldOut = makeFrame(m, bitstest::randomBytes(rng, len), order);
                const auto hits = findCrc(frames);
                ++tried;
                bool pg = false, hg = false;
                for (const auto& h : hits) {
                    for (const auto& f : frames) CHECK(reproduces(h.model, f, h.order, h.headBytes, h.tailBytes));  // never a false claim
                    if (h.model.width == w && h.model.poly == m.poly && h.model.refin == m.refin && h.model.refout == m.refout && h.order == order) {
                        pg = true;
                        // one-length frames: init and xorout are an equivalent choice, so the held-out frame of the SAME length must check
                        hg |= reproduces(h.model, heldOut, order, 0, 0);
                        CHECK(h.freeDims > 0 || h.model.init == m.init);
                        CHECK(h.freeDims == w);  // one length: exactly w dimensions undetermined
                        CHECK(h.note.find("cannot be told apart") != std::string::npos);
                    }
                    // (A frame of a 32-bit CRC also satisfies a shorter-width reading whenever the generator factors; only a hit at the TRUE width counts here.)
                    if (h.derived && h.candidates == 1 && h.model.width == w) {
                        ++uniqueClaims;
                        uniqueRight += (h.model.poly == m.poly && h.model.refin == m.refin && h.model.refout == m.refout) ? 1 : 0;
                    }
                }
                polyOk += pg ? 1 : 0;
                heldOk += hg ? 1 : 0;
            }
        std::printf("  random definitions (5 frames, one length): polynomial found %d/%d, held-out frame reproduced %d/%d, "
                    "unique claims %d of which right %d\n", polyOk, tried, heldOk, tried, uniqueClaims, uniqueRight);
        CHECK(polyOk == tried);
        CHECK(heldOk == tried);
        CHECK(uniqueClaims > 0 && uniqueRight == uniqueClaims);
    }

    // ---- two different lengths settle init and final XOR exactly -------------------------------------------
    {
        int ok = 0;
        const int trials = 20;
        for (int t = 0; t < trials; ++t) {
            CrcModel m{16, static_cast<uint64_t>((rng.next() & 0xFFFF) | 1u), rng.next() & 0xFFFF, rng.next() & 0xFFFF, rng.below(2) != 0, rng.below(2) != 0, ""};
            std::vector<std::vector<uint8_t>> frames;
            for (size_t len : {9u, 9u, 9u, 15u, 15u, 15u}) frames.push_back(makeFrame(m, bitstest::randomBytes(rng, len), ByteOrder::Big));
            const auto hits = findCrc(frames);
            for (const auto& h : hits)
                if (h.derived && h.model.poly == m.poly && h.model.refin == m.refin && h.model.refout == m.refout && h.order == ByteOrder::Big) {
                    // with two lengths the system has rank 2w minus at most the one collapsed dimension class; the init must be the true one when freeDims == 0
                    if (h.freeDims == 0) {
                        CHECK(h.model.init == m.init && h.model.xorout == m.xorout);
                        // a third length checks
                        CHECK(reproduces(h.model, makeFrame(m, bitstest::randomBytes(rng, 23), ByteOrder::Big), ByteOrder::Big, 0, 0));
                        ++ok;
                    } else {
                        CHECK(h.freeDims < 16);
                    }
                }
        }
        std::printf("  two lengths: init and xorout determined exactly in %d/%d random definitions\n", ok, trials);
        // Exact only when (1 + x^d) is coprime to the generator (d = the length difference in bits): about half of random generators.
        CHECK(ok >= 3 && ok < trials);
    }

    // ---- TWO frames of one length: nothing is derived (one difference), nothing false is claimed ------------
    {
        const CrcModel odd{16, 0x0589, 0x1234, 0x00ff, false, true, ""};
        std::vector<std::vector<uint8_t>> frames;
        for (int i = 0; i < 2; ++i) frames.push_back(makeFrame(odd, bitstest::randomBytes(rng, 6), ByteOrder::Big));
        const auto hits = findCrc(frames);
        for (const auto& h : hits) {
            for (const auto& f : frames) CHECK(reproduces(h.model, f, h.order, h.headBytes, h.tailBytes));
            CHECK(!h.derived);  // one difference is not enough to derive a polynomial
        }
    }

    // ---- frames with NO CRC: nothing is claimed ---------------------------------------------------------------
    {
        size_t falseHits = 0, sets = 0;
        CrcFindOptions o16;
        o16.widths = {16, 32};
        for (int t = 0; t < 40; ++t) {
            std::vector<std::vector<uint8_t>> frames;
            const size_t len = 10 + rng.below(10);
            for (int i = 0; i < 5; ++i) frames.push_back(bitstest::randomBytes(rng, len));
            falseHits += findCrc(frames, o16).size();
            ++sets;
        }
        std::printf("  random frames, widths 16/32: %zu false hits in %zu sets\n", falseHits, sets);
        CHECK(falseHits == 0);
        CrcFindOptions o8;
        o8.widths = {8};
        size_t fh8 = 0;
        for (int t = 0; t < 100; ++t) {
            std::vector<std::vector<uint8_t>> frames;
            for (int i = 0; i < 5; ++i) frames.push_back(bitstest::randomBytes(rng, 12));
            fh8 += findCrc(frames, o8).size();
        }
        std::printf("  random frames, width 8, five frames: %zu false hits in 100 sets\n", fh8);
        CHECK(fh8 <= 1);
        // a damaged frame: the CRC no longer holds for every frame, so it is not reported
        const CrcModel m = byName("MODBUS")->model;
        std::vector<std::vector<uint8_t>> frames;
        for (int i = 0; i < 4; ++i) frames.push_back(makeFrame(m, bitstest::randomBytes(rng, 12), ByteOrder::Little));
        frames[2][3] ^= 0x10;
        bool named = false;
        for (const auto& h : findCrc(frames)) named |= h.catalogueMatch && std::strstr(h.model.name, "MODBUS");
        CHECK(!named);
        // options bound the work: a frame over maxFrameBytes is ignored
        CrcFindOptions small;
        small.maxFrameBytes = 4;
        CHECK(findCrc(frames, small).empty());
        CrcFindOptions limited;
        limited.maxHits = 1;
        CHECK(findCrc({makeFrame(m, bitstest::randomBytes(rng, 12), ByteOrder::Little), makeFrame(m, bitstest::randomBytes(rng, 12), ByteOrder::Little)}, limited).size() <= 1);
    }

    // ---- gf(2) arithmetic ----------------------------------------------------------------------------------------
    {
        gf2::Poly a{0b1011}, b{0b11};  // x^3 + x + 1, x + 1
        CHECK(gf2::degree(a) == 3 && gf2::degree(b) == 1 && gf2::degree({}) == -1);
        // (x^3 + x + 1) mod (x + 1) = 1 (value at x = 1 is 1 + 1 + 1 = 1)
        const auto r = gf2::mod(a, b);
        CHECK(r.size() == 1 && r[0] == 1);
        // gcd((x+1)(x^2+x+1), (x+1)(x^3+x+1)) = x+1
        gf2::Poly p1{0b111}, p2{0b1011}, ab{0b11};
        gf2::Poly x1, x2;
        gf2::xorShifted(x1, p1, 0);
        gf2::xorShifted(x1, p1, 1);  // (x^2+x+1)(x+1)
        gf2::xorShifted(x2, p2, 0);
        gf2::xorShifted(x2, p2, 1);
        const auto g = gf2::gcd(x1, x2);
        CHECK(g.size() == 1 && g[0] == 0b11);
        (void)ab;
        gf2::Poly big;
        gf2::flip(big, 200);
        gf2::flip(big, 0);
        CHECK(gf2::degree(big) == 200 && gf2::coeff(big, 200) && gf2::coeff(big, 0) && !gf2::coeff(big, 100));
    }

    // ---- additive, XOR, Fletcher, Internet checksums ----------------------------------------------------------------
    {
        // Known answers
        const auto d9 = asciiDigits();
        CHECK(sumCompute(SumKind::Sum8, d9.data(), d9.size()) == 0xDD);        // 0x31+...+0x39 = 477 = 0x1DD
        CHECK(sumCompute(SumKind::Sum8Neg, d9.data(), d9.size()) == 0x23);
        CHECK(sumCompute(SumKind::Xor8, d9.data(), d9.size()) == 0x31);        // 1^2^...^9 over the ASCII digits
        CHECK(sumCompute(SumKind::Sum16Bytes, d9.data(), d9.size()) == 477);
        CHECK(sumCompute(SumKind::NibbleSum, d9.data(), d9.size()) == 3 * 9 + 45);
        // Fletcher-16 of "abcde" is 0xC8F0 (Fletcher's published example vector for mod 255 with initial sums 0)
        const uint8_t abcde[] = {'a', 'b', 'c', 'd', 'e'};
        CHECK(sumCompute(SumKind::Fletcher16, abcde, 5) == 0xC8F0);
        // RFC 1071 example: words 0001 f203 f4f5 f6f7 -> sum ddf2, checksum 220d
        const uint8_t rfc[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
        CHECK(sumCompute(SumKind::Internet16, rfc, 8) == 0x220d);
        // Round trip through the finder for every kind, both byte orders, with head and tail bytes
        for (int ki = 0; ki <= static_cast<int>(SumKind::Internet16); ++ki) {
            const SumKind k = static_cast<SumKind>(ki);
            for (ByteOrder o : {ByteOrder::Big, ByteOrder::Little}) {
                const unsigned nb = sumBytes(k);
                if (nb == 1 && o == ByteOrder::Little) continue;
                std::vector<std::vector<uint8_t>> frames;
                for (int i = 0; i < 4; ++i) {
                    std::vector<uint8_t> f = {0x7E, 0x01};
                    const auto body = bitstest::randomBytes(rng, 10 + 2 * rng.below(4));  // even length keeps the 16-bit word kinds exact
                    f.insert(f.end(), body.begin(), body.end());
                    const uint32_t c = sumCompute(k, f.data() + 2, body.size());
                    for (unsigned j = 0; j < nb; ++j) f.push_back(static_cast<uint8_t>(c >> (8 * (o == ByteOrder::Big ? nb - 1 - j : j))));
                    f.push_back(0x0A);
                    frames.push_back(std::move(f));
                }
                const auto hits = findChecksums(frames);
                bool good = false;
                for (const auto& h : hits) good |= h.kind == k && h.order == o && h.headBytes == 2 && h.tailBytes == 1;
                if (!good) std::printf("  checksum not found: %s\n", sumName(k));
                CHECK(good);
            }
        }
        // Random frames: no checksum is claimed (one-byte kinds can coincide with chance 1/256 per layout; 5 frames make it negligible)
        size_t fh = 0;
        for (int t = 0; t < 50; ++t) {
            std::vector<std::vector<uint8_t>> frames;
            for (int i = 0; i < 5; ++i) frames.push_back(bitstest::randomBytes(rng, 12));
            fh += findChecksums(frames).size();
        }
        CHECK(fh == 0);
        CHECK(findChecksums({}).empty());
        CHECK(findChecksums({{1}}).empty());
        CHECK(sumCompute(SumKind::Sum8, nullptr, 0) == 0 && sumCompute(SumKind::Internet16, nullptr, 0) == 0xFFFF);
    }
    return testSummary("test_bits_crc");
}
