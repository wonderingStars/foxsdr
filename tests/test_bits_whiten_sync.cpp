// Bit-view library: whitening/scramblers (whiten.hpp), preamble and sync search, framing (sync.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>

#include "bits_test_util.hpp"
#include "core/bits/bits.hpp"
#include "core/bits/sync.hpp"
#include "core/bits/whiten.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {
Bits B(const char* s) { return fromString(s); }
}  // namespace

int main() {
    Rng rng(0xFEED);

    // ---- bit helpers ---------------------------------------------------------------------------
    {
        bool ok = false;
        CHECK(toString(fromString("1011 0001", &ok)) == "10110001" && ok);
        fromString("10x1", &ok);
        CHECK(!ok);
        const std::vector<uint8_t> by = {0xA5, 0x0F, 0x80};
        CHECK(toBytes(fromBytes(by)) == by);
        CHECK(toBytes(fromBytes(by, false), false) == by);
        CHECK(toBytes(B("101")) == std::vector<uint8_t>({0xA0}));
        CHECK(getField(B("1011"), 0, 4) == 11 && getField(B("1011"), 0, 4, false) == 13);
        CHECK(getField(B("1"), 0, 8) == 0x80);  // past the end reads as 0
        CHECK(slice(B("1011"), 2, 99) == B("11") && slice(B("1011"), 9, 1).empty());
        CHECK(hamming(B("1010"), B("1001")) == 2 && hamming(B("1"), B("111")) == 2);
        CHECK(toString(reverseBitsInBytes(B("1000 0000 11"))) == "0000000111");
        CHECK(toString(B("11110000"), 4) == "1111 0000");
    }

    // ---- PN9 additive whitening ----------------------------------------------------------------
    {
        // The register's first sixteen bytes (x^9 + x^5 + 1, all ones, 8 LSBs per byte, one clock per bit).
        const std::vector<uint8_t> want = {0xFF, 0xE1, 0x1D, 0x9A, 0xED, 0x85, 0x33, 0x24, 0xEA, 0x7A, 0xD2, 0x39, 0x70, 0x97, 0x57, 0x0A};
        const auto got = whitenBytes(std::vector<uint8_t>(16, 0), kPn9, kPn9Seed);
        CHECK(got == want);
        // Maximal length: the 9-bit register repeats after exactly 511 bits and not before.
        const Bits seq = lfsrSequence(kPn9, kPn9Seed, 2 * 511 + 9);
        bool periodic = true;
        for (size_t i = 0; i + 511 < seq.size(); ++i) periodic &= seq[i] == seq[i + 511];
        CHECK(periodic);
        bool shorter = false;
        for (size_t p = 1; p < 511; ++p) {
            bool same = true;
            for (size_t i = 0; i < 600 && same; ++i) same &= seq[i] == seq[i + p];
            shorter |= same;
        }
        CHECK(!shorter);
        // An involution: whitening twice returns the data, bytes and bits.
        const auto data = bitstest::randomBytes(rng, 64);
        CHECK(whitenBytes(whitenBytes(data, kPn9, kPn9Seed), kPn9, kPn9Seed) == data);
        const Bits db = bitstest::randomBits(rng, 333);
        CHECK(whitenBits(whitenBits(db, kPn9, kPn9Seed), kPn9, kPn9Seed) == db);
        CHECK(whitenBits(db, kPn9, kPn9Seed) != db);
        // The alternative x^9 + x^4 + 1 (also maximal) gives a different sequence: the known-answer row has teeth.
        const LfsrSpec alt{9, (1ull << 0) | (1ull << 4)};
        CHECK(whitenBytes(std::vector<uint8_t>(16, 0), alt, kPn9Seed) != want);
        // A different seed shifts the sequence.
        CHECK(whitenBytes(std::vector<uint8_t>(4, 0), kPn9, 0x0AA) != whitenBytes(std::vector<uint8_t>(4, 0), kPn9, kPn9Seed));
        // The chain wrapper agrees.
        WhitenSetting ws;
        ws.kind = WhitenKind::AdditiveBytes;
        ws.lfsr = kPn9;
        ws.seed = kPn9Seed;
        CHECK(toBytes(applyWhitening(fromBytes(data), ws)) == whitenBytes(data, kPn9, kPn9Seed));
        // A short tail of < 8 bits passes through.
        Bits tail = fromBytes(data);
        tail.push_back(1);
        tail.push_back(0);
        const Bits w = applyWhitening(tail, ws);
        CHECK(w.size() == tail.size() && w[w.size() - 2] == 1 && w[w.size() - 1] == 0);
        // Invalid specs hand the data back unchanged (never crash, never an all-zero sequence).
        CHECK(!lfsrValid({0, 1}, 1) && !lfsrValid({9, 0x21}, 0) && !lfsrValid({64, 1}, 1) && !lfsrValid({9, 1ull << 9}, 1));
        CHECK(lfsrSequence({9, 0x21}, 0, 16).empty());
        CHECK(whitenBits(db, {9, 0x21}, 0) == db);
        CHECK(whitenBytes(data, {0, 0}, 1) == data);
        CHECK(lfsrSequence(kPn9, kPn9Seed, 0).empty());
        CHECK(whitenBits({}, kPn9, kPn9Seed).empty());
    }

    // ---- self-synchronising scramblers (G3RUH 1 + x^12 + x^17) -----------------------------------
    {
        const Bits data = bitstest::randomBits(rng, 600);
        const Bits sc = scramble(data, kG3ruh);
        CHECK(sc != data);
        CHECK(descramble(sc, kG3ruh) == data);
        // The defining relation, stated bit by bit: sent[i] = data[i] ^ sent[i-12] ^ sent[i-17].
        bool rel = true;
        for (size_t i = 17; i < sc.size(); ++i) rel &= sc[i] == (data[i] ^ sc[i - 12] ^ sc[i - 17]);
        CHECK(rel);
        // SELF-SYNCHRONISATION: join the stream late and the descrambler locks after 17 bits.
        const Bits late(sc.begin() + 100, sc.end());
        const Bits dl = descramble(late, kG3ruh);
        bool locked = true;
        for (size_t i = 17; i < dl.size(); ++i) locked &= dl[i] == data[100 + i];
        CHECK(locked);
        // One channel bit error costs three data errors (the bit itself and the two taps).
        Bits bad = sc;
        bad[300] ^= 1u;
        const Bits db = descramble(bad, kG3ruh);
        size_t diff = 0;
        for (size_t i = 0; i < db.size(); ++i) diff += db[i] != data[i];
        CHECK(diff == 3);
        CHECK(descramble(scramble(data, kV27ter), kV27ter) == data);
        CHECK(descramble(scramble(data, kV29), kV29) == data);
        CHECK(!tapsValid({0, 5}) && !tapsValid({5, 5}) && !tapsValid({9, 3}) && !tapsValid({1, 65}) && tapsValid({1, 64}));
        CHECK(descramble(data, {0, 0}) == data && scramble(data, {9, 3}) == data);  // invalid taps: unchanged
        CHECK(scramble({}, kG3ruh).empty() && descramble({}, kG3ruh).empty());
        WhitenSetting ws;
        ws.kind = WhitenKind::SelfSync;
        ws.taps = kG3ruh;
        CHECK(applyWhitening(sc, ws) == data);
        CHECK(applyWhitening(sc, WhitenSetting{}) == sc);
    }

    // ---- pattern search with a Hamming tolerance --------------------------------------------------
    {
        const Bits sync = B("0010 1101 1101 0100");  // 2DD4
        Bits stream = bitstest::randomBits(rng, 40);
        const size_t at = stream.size();
        stream.insert(stream.end(), sync.begin(), sync.end());
        const Bits payload = bitstest::randomBits(rng, 32);
        stream.insert(stream.end(), payload.begin(), payload.end());
        auto h = findPattern(stream, sync, 0);
        // Random surroundings could in principle hold a second exact hit; ours must be there at the right offset.
        bool found = false;
        for (const auto& x : h) found |= x.pos == at && x.distance == 0;
        CHECK(found);
        // With errors in the sync word: absent at tolerance 0, present at the right tolerance, with the right distance.
        Bits damaged = stream;
        damaged[at + 3] ^= 1u;
        damaged[at + 9] ^= 1u;
        auto h0 = findPattern(damaged, sync, 0);
        bool f0 = false;
        for (const auto& x : h0) f0 |= x.pos == at;
        CHECK(!f0);
        auto h1 = findPattern(damaged, sync, 1);
        bool f1 = false;
        for (const auto& x : h1) f1 |= x.pos == at;
        CHECK(!f1);
        auto h2 = findPattern(damaged, sync, 2);
        bool f2 = false;
        for (const auto& x : h2) if (x.pos == at) { f2 = true; CHECK(x.distance == 2); }
        CHECK(f2);
        // Inverted polarity.
        auto hi = findPattern(inverted(stream), sync, 0, true);
        bool fi = false;
        for (const auto& x : hi) if (x.pos == at) { fi = x.inverted && x.distance == 0; }
        CHECK(fi);
        bool fni = false;
        for (const auto& x : findPattern(inverted(stream), sync, 0, false)) fni |= x.pos == at;
        CHECK(!fni);  // without the inverted option the inverted word is not found
        // Repeating pattern: the collapse keeps one hit per pattern length; an exact repeat 1 bit later is not a hit of the same word.
        const Bits rep = B("1010 1010 1010 1010");
        auto hr = findPattern(rep, B("1010"), 0);
        CHECK(hr.size() == 4);  // positions 0, 4, 8, 12 (2, 6, 10 collapse into their neighbours)
        // Edges and degenerate inputs.
        CHECK(findPattern({}, sync).empty() && findPattern(stream, {}).empty());
        CHECK(findPattern(B("101"), sync).empty());
        CHECK(findPattern(stream, Bits(5000, 1)).empty());
        CHECK(findPattern(rep, B("1010"), 0, false, 2).size() == 2);  // maxHits
        const auto tail = findPattern(B("0001 0110"), B("0110"), 0);
        CHECK(tail.size() == 1 && tail[0].pos == 4);  // a match ending exactly at the end is found
    }

    // ---- preamble measurement ---------------------------------------------------------------------
    {
        const auto p = measurePreamble(B("1010101010101010 0010 1101"));
        CHECK(p.kind == PreambleRun::Alternating);
        CHECK(p.length == 16);  // the run stops where the sync word repeats the last preamble bit (0, 0)
        const auto c = measurePreamble(B("11111111 11111111 0100"));
        CHECK(c.kind == PreambleRun::Constant && c.length == 16);
        CHECK(measurePreamble(B("1101")).kind == PreambleRun::None);
        CHECK(measurePreamble({}).length == 0 && measurePreamble(B("1010"), 9).length == 0);
        CHECK(measurePreamble(B("0101010101010101"), 0, 8).kind == PreambleRun::Alternating);
        CHECK(measurePreamble(B("0101"), 0, 8).kind == PreambleRun::None);
    }

    // ---- framing: fixed size, length field, flag (HDLC stuffing) -----------------------------------
    {
        const Bits sync = B("0011 0101 1001 0111");
        Bits stream;
        std::vector<Bits> payloads;
        for (int f = 0; f < 5; ++f) {
            const Bits pre = bitstest::randomBits(rng, 8 + rng.below(20));
            stream.insert(stream.end(), pre.begin(), pre.end());
            stream.insert(stream.end(), sync.begin(), sync.end());
            // payload: 8-bit length field (in bytes, counting the length byte and the body) + body
            const size_t body = 3 + rng.below(6);
            Bits p;
            for (int k = 7; k >= 0; --k) p.push_back(static_cast<uint8_t>(((body + 1) >> k) & 1u));
            const Bits bb = bitstest::randomBits(rng, body * 8);
            p.insert(p.end(), bb.begin(), bb.end());
            payloads.push_back(p);
            stream.insert(stream.end(), p.begin(), p.end());
        }
        const auto hits = findPattern(stream, sync, 0);
        CHECK(hits.size() >= 5);
        LengthFraming lf;  // field at 0, 8 bits, unit 8 bits: total = (len) * 8
        size_t rej = 99;
        const auto frames = frameByLength(stream, hits, sync.size(), lf, &rej);
        size_t matched = 0;
        for (const auto& pl : payloads)
            for (const auto& f : frames) matched += f.bits == pl;
        CHECK(matched == 5);
        // Length field pointing past the stream, zero, or over the limit is rejected, not read.
        Bits cut(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(stream.size() - 20));
        size_t rej2 = 0;
        const auto fcut = frameByLength(cut, findPattern(cut, sync, 0), sync.size(), lf, &rej2);
        CHECK(rej2 >= 1 && fcut.size() + rej2 >= 5 - 1);
        Bits zero = sync;
        for (int k = 0; k < 8; ++k) zero.push_back(0);
        zero.insert(zero.end(), 40, 1);
        size_t rej3 = 0;
        CHECK(frameByLength(zero, findPattern(zero, sync, 0), sync.size(), lf, &rej3).empty() && rej3 == 1);
        LengthFraming lfmax = lf;
        lfmax.maxBits = 16;
        size_t rej4 = 0;
        const auto fm = frameByLength(stream, hits, sync.size(), lfmax, &rej4);
        CHECK(rej4 >= 1);
        for (const auto& f : fm) CHECK(f.bits.size() <= 16);
        LengthFraming bad = lf;
        bad.lenBits = 0;
        size_t rej5 = 0;
        CHECK(frameByLength(stream, hits, sync.size(), bad, &rej5).empty() && rej5 == hits.size());
        bad.lenBits = 33;
        CHECK(frameByLength(stream, hits, sync.size(), bad).empty());
        // Fixed size.
        const auto fx = frameFixed(stream, hits, sync.size(), 16);
        CHECK(fx.size() == hits.size());
        for (const auto& f : fx) CHECK(f.bits.size() == 16);
        const auto fshort = frameFixed(stream, hits, sync.size(), 100000);
        CHECK(fshort.empty());
        CHECK(frameFixed(stream, hits, sync.size(), 100000, true).size() == hits.size());

        // Flag-delimited with bit stuffing: a body full of ones survives the round trip.
        const Bits flag = B("01111110");
        Bits tx;
        std::vector<Bits> bodies = {B("1111 1111 1111 1111 0000 1111 1"), bitstest::randomBits(rng, 77), B("1"), Bits(40, 1)};
        for (const auto& b : bodies) {
            const Bits s = stuffFrame(b, flag);
            // consecutive frames share nothing: a single flag between frames is enough
            tx.insert(tx.end(), s.begin(), s.end());
        }
        size_t ab = 0;
        const auto ff = frameByFlag(tx, flag, true, 4096, &ab);
        // Back-to-back frames: flag-body-flag flag-body-flag leaves an empty frame between each pair, which is dropped.
        size_t exact = 0;
        for (const auto& b : bodies)
            for (const auto& f : ff) exact += f.bits == b;
        CHECK(exact >= bodies.size() - 1);  // random bodies could in principle hold the flag; ours are checked below
        CHECK(ab == 0);
        // Without un-stuffing the stuffed zeros stay.
        const auto raw = frameByFlag(stuffFrame(Bits(20, 1), flag), flag, false);
        CHECK(raw.size() == 1 && raw[0].bits.size() == 24);
        const auto uns = frameByFlag(stuffFrame(Bits(20, 1), flag), flag, true);
        CHECK(uns.size() == 1 && uns[0].bits == Bits(20, 1));
        // Six ones inside a frame that are not a flag abort it.
        Bits abort = flag;
        const Bits ones7 = B("0 1111111 0");
        abort.insert(abort.end(), ones7.begin(), ones7.end());
        abort.insert(abort.end(), flag.begin(), flag.end());
        size_t ab2 = 0;
        // ones7 holds 7 ones: the flag search sees 01111110? "0 1111111 0" contains 0111111 1: no flag; the unstuffer aborts.
        CHECK(frameByFlag(abort, flag, true, 4096, &ab2).empty() && ab2 == 1);
        CHECK(frameByFlag({}, flag, true).empty() && frameByFlag(tx, {}, true).empty());
        CHECK(frameByFlag(stuffFrame(Bits(200, 0), flag), flag, true, 100).empty());  // over maxBits
    }
    return testSummary("test_bits_whiten_sync");
}
