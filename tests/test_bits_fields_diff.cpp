// Bit-view library: automatic field inference (fields.hpp) and message diffing (msgdiff.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>

#include "bits_test_util.hpp"
#include "core/bits/crc.hpp"
#include "core/bits/fields.hpp"
#include "core/bits/msgdiff.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {
Bits B(const char* s) { return fromString(s); }

const InferredField* at(const std::vector<InferredField>& v, size_t pos) {
    for (const auto& f : v) if (f.pos == pos) return &f;
    return nullptr;
}
}  // namespace

int main() {
    Rng rng(0xF1E1D);

    // ---- diffBits ---------------------------------------------------------------------------------
    {
        const Bits a = B("1011 0010 1110 0101");
        auto d = diffBits(a, a);
        CHECK(d.ok && d.matches == 16 && d.mismatches == 0 && d.gaps == 0 && d.runs.empty());
        CHECK(d.rowA == "1011001011100101" && d.marks == "||||||||||||||||");
        // one flipped bit: one mismatch, one run of length 1 at the right place
        Bits b = a;
        b[6] ^= 1u;
        d = diffBits(a, b);
        CHECK(d.mismatches == 1 && d.gaps == 0 && d.runs.size() == 1 && d.runs[0].aStart == 6 && d.runs[0].aLen == 1 && d.runs[0].bLen == 1);
        CHECK(d.marks[6] == 'x');
        // an inserted bit is found as a gap, not as a cascade of mismatches (random bits give it a unique alignment)
        const Bits base = bitstest::randomBits(rng, 120);
        Bits ins = base;
        size_t ip = 50;
        while (ip < 100 && base[ip - 1] != base[ip]) ++ip;  // between two equal bits, insert the opposite: one alignment only
        ins.insert(ins.begin() + static_cast<std::ptrdiff_t>(ip), static_cast<uint8_t>(base[ip] ^ 1u));
        d = diffBits(base, ins);
        CHECK(d.gaps == 1 && d.mismatches == 0 && d.matches == 120);
        CHECK(d.runs.size() == 1 && d.runs[0].aLen == 0 && d.runs[0].bLen == 1 && d.runs[0].bStart == ip);
        // without gaps the same pair is a heap of mismatches after the insertion
        const auto dn = diffBits(base, ins, false);
        CHECK(dn.mismatches > 20);
        // deletion, and both strings empty / one empty
        Bits del = base;
        del.erase(del.begin() + 70);
        d = diffBits(base, del);
        CHECK(d.gaps == 1 && d.mismatches == 0 && d.runs[0].aLen == 1 && d.runs[0].bLen == 0);
        d = diffBits({}, {});
        CHECK(d.ok && d.ops.empty() && d.runs.empty());
        d = diffBits(B("101"), {});
        CHECK(d.ok && d.gaps == 3 && d.rowB == "---" && d.runs.size() == 1);
        // the limit: refused, not attempted
        CHECK(!diffBits(Bits(3000, 0), Bits(10, 1)).ok);
        // random pairs: the counts are consistent whatever the alignment
        for (int t = 0; t < 20; ++t) {
            const Bits x = bitstest::randomBits(rng, 10 + rng.below(80)), y = bitstest::randomBits(rng, 10 + rng.below(80));
            d = diffBits(x, y);
            size_t ia = 0, ib = 0;
            for (DiffOp op : d.ops) {
                if (op != DiffOp::OnlyB) ++ia;
                if (op != DiffOp::OnlyA) ++ib;
            }
            CHECK(ia == x.size() && ib == y.size());
            CHECK(d.matches + d.mismatches + d.gaps == d.ops.size());
        }
    }

    // ---- column statistics and byte diff -------------------------------------------------------------------
    {
        const auto st = diffFrames({B("1010"), B("1001"), B("10")});
        CHECK(st.size() == 4 && st[0].constant() && st[1].constant() && st[0].count == 3 && st[2].count == 2 && !st[2].constant() && st[3].count == 2);
        CHECK(diffFrames({}).empty());
        const std::string t = diffBytesText({0x01, 0x02, 0x03}, {0x01, 0xFF});
        CHECK(t == "01 02 03 \n01 FF -- \n   ^^ ^^ \n");
    }

    // ---- field inference: constants, counter, flags, data, checksum ---------------------------------------------
    {
        // layout (bytes): 0 = 0xA5 constant, 1 = type 0x10 constant, 2 = counter (+1), 3 = flags (2 values), 4-5 = data, 6 = CRC-8, but frames carry a CRC-8/SMBUS
        const CrcModel crc8 = crcCatalogue()[0].model;
        std::vector<Bits> frames;
        for (int i = 0; i < 40; ++i) {
            std::vector<uint8_t> f = {0xA5, 0x10, static_cast<uint8_t>(7 + i), static_cast<uint8_t>(rng.below(2) ? 0x80 : 0x01), rng.byte(), rng.byte()};
            f.push_back(static_cast<uint8_t>(crcBytes(crc8, f)));
            frames.push_back(fromBytes(f));
        }
        const auto fs = inferFields(frames);
        // The constants: the 16 bits at 0 (0xA5 0x10)
        const auto* c0 = at(fs, 0);
        CHECK(c0 && c0->kind == FieldKind::Constant && c0->len == 16 && c0->value == 0xA510);
        const auto* ctr = at(fs, 16);
        CHECK(ctr && ctr->kind == FieldKind::Counter && ctr->len == 8 && ctr->value == 1 && ctr->distinct == 40);
        const auto* fl = at(fs, 24);
        CHECK(fl && fl->kind == FieldKind::Flags && fl->distinct == 2);
        // two random bytes: data (varying, many values), and the last byte is the checksum
        const auto* d1 = at(fs, 32);
        CHECK(d1 && d1->kind == FieldKind::Data && d1->len == 8);
        const auto* ck = at(fs, 48);
        CHECK(ck && ck->kind == FieldKind::Checksum && ck->len == 8);
        if (ck) CHECK(ck->note.find("CRC-8/SMBUS") != std::string::npos);
        // fields tile the frame in order, without overlap
        size_t cursor = 0;
        for (const auto& f : fs) { CHECK(f.pos >= cursor); cursor = f.pos + f.len; }
        // a counter with a missing frame (steps of 1 with a few of 2): still a counter, and the note says so
        std::vector<Bits> gappy;
        int v = 0;
        for (int i = 0; i < 40; ++i) {
            v += (i % 13 == 12) ? 2 : 1;
            gappy.push_back(fromBytes({0x55, static_cast<uint8_t>(v)}));
        }
        const auto fg = inferFields(gappy);
        const auto* g = at(fg, 8);
        CHECK(g && g->kind == FieldKind::Counter && g->value == 1 && g->note.find("of") != std::string::npos);
        // a 16-bit counter with a constant high byte
        std::vector<Bits> c16;
        for (int i = 0; i < 20; ++i) c16.push_back(fromBytes({0x03, static_cast<uint8_t>(100 + 3 * i), 0x77}));
        const auto f16 = inferFields(c16);
        bool counterFound = false;
        for (const auto& f : f16) counterFound |= f.kind == FieldKind::Counter && f.value == 3;
        CHECK(counterFound);
    }

    // ---- a length field, for frames of differing length ---------------------------------------------------------------
    {
        std::vector<Bits> frames;
        for (int i = 0; i < 20; ++i) {
            const size_t n = 4 + rng.below(8);
            std::vector<uint8_t> f = {0x7E, static_cast<uint8_t>(n + 2)};  // length counts the whole frame in bytes incl. these two
            for (size_t k = 0; k < n; ++k) f.push_back(rng.byte());
            frames.push_back(fromBytes(f));
        }
        const auto fs = inferFields(frames);
        const auto* l = at(fs, 8);
        CHECK(l && l->kind == FieldKind::Length && l->lengthInBytes && l->value == 0);
        // Same frames of one length: no length field is claimed (nothing to tell it from data)
        std::vector<Bits> same;
        for (int i = 0; i < 20; ++i) same.push_back(fromBytes({0x7E, 0x0A, rng.byte(), rng.byte()}));
        for (const auto& f : inferFields(same)) CHECK(f.kind != FieldKind::Length);
    }

    // ---- no frames, one frame, ragged input, random frames ---------------------------------------------------------------
    {
        CHECK(inferFields({}).empty());
        CHECK(inferFields({B("1010")}).empty());
        CHECK(inferFields({Bits{}, Bits{}}).empty());
        std::vector<Bits> rnd;
        for (int i = 0; i < 30; ++i) rnd.push_back(bitstest::randomBits(rng, 64));
        const auto fs = inferFields(rnd);
        size_t covered = 0;
        for (const auto& f : fs) {
            CHECK(f.kind != FieldKind::Checksum && f.kind != FieldKind::Counter);  // nothing structural in noise (16-bit checksum chance is 2^-16)
            covered += f.len;
        }
        CHECK(covered == 64);
        // ragged: only the common length is examined
        const auto fr = inferFields({B("1111 0000 1"), B("1111 0000"), B("1111 0000 111")});
        CHECK(!fr.empty() && fr[0].kind == FieldKind::Constant && fr[0].len == 8);
        // resource limits: frames beyond maxBits are ignored
        InferOptions o;
        o.maxBits = 8;
        CHECK(inferFields({Bits(16, 1), Bits(16, 1)}, o).empty());
    }
    return testSummary("test_bits_fields_diff");
}
