// BIT-VIEW LIBRARY, part 5: CRC model and engine, a catalogue of published definitions (DATA only),
// a finder that recovers the definition from sample frames, and additive / XOR / Fletcher checksums.
//
// WHERE THIS COMES FROM. The CRC arithmetic is the textbook one (Williams, "A Painless Guide to CRC
// Error Detection Algorithms": register, polynomial, init, input and output reflection, final XOR).
// The FINDER is written here from the algebra and uses no other program:
//
//   For frames of equal length the CRC is affine over GF(2): crc(a) ^ crc(b) depends on the
//   polynomial only (init and final XOR cancel). So for two equal-length frames a, b with check
//   values ca, cb, the polynomial
//       E(x) = (a ^ b)(x) * x^w  +  (ca ^ cb)(x)
//   is divisible by the generator g(x) (degree w). The gcd over GF(2) of the E of every pair
//   in a group of equal-length frames is therefore a multiple of g; when its degree is exactly w it
//   IS g (up to the implied x^w term). A larger gcd leaves a few extra factors: the finder tries to
//   remove them (all odd cofactors up to degree 16), keeps every degree-w divisor and says so - that
//   is the honest ambiguity. With g known, init and final XOR follow from a linear system over
//   GF(2) (one equation set per frame); frames of one single length cannot tell init from final
//   XOR (the system is under-determined by exactly w dimensions), and the result says that too,
//   naming a catalogue definition when one reproduces every frame.
//
//   The reveng program and its source were not opened. The catalogue below is DATA: the parameters
//   and check values for "123456789" printed in the public CRC catalogue (reveng.sourceforge.io/
//   crc-catalogue/all.htm, opened 2026-11-11, page states no licence); test_bits_crc asserts every
//   entry's check value against this engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/bits/bits.hpp"

namespace cascade::bits {

struct CrcModel {
    unsigned width = 0;  // 1..64
    uint64_t poly = 0;   // without the x^width term
    uint64_t init = 0;
    uint64_t xorout = 0;
    bool refin = false;
    bool refout = false;
    const char* name = "";
};

inline uint64_t crcMask(unsigned w) { return w >= 64 ? ~0ull : ((1ull << w) - 1); }

inline uint64_t reflectBits(uint64_t v, unsigned w) {
    uint64_t r = 0;
    for (unsigned i = 0; i < w; ++i)
        if (v >> i & 1u) r |= 1ull << (w - 1 - i);
    return r;
}

// The raw register after feeding `bits` (in feed order, MSB first polynomial arithmetic), from `init`,
// with no reflection and no final XOR: reg = bits(x) * x^w + init-term mod g.
inline uint64_t crcRegister(unsigned w, uint64_t poly, uint64_t init, const Bits& bits) {
    const uint64_t mask = crcMask(w);
    uint64_t reg = init & mask;
    const uint64_t top = 1ull << (w - 1);
    for (uint8_t b : bits) {
        const bool fb = ((reg & top) != 0) != (b != 0);
        reg = (reg << 1) & mask;
        if (fb) reg ^= poly;
    }
    return reg;
}

inline bool crcModelValid(const CrcModel& m) { return m.width >= 1 && m.width <= 64; }

// The CRC of a message given as bits in transmission order. With refin the bits of each byte are
// fed least significant first, which needs a whole number of bytes: otherwise ok is false.
inline uint64_t crcCompute(const CrcModel& m, const Bits& msg, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (!crcModelValid(m)) { if (ok) *ok = false; return 0; }
    if (m.refin && msg.size() % 8 != 0) { if (ok) *ok = false; return 0; }
    const Bits fed = m.refin ? reverseBitsInBytes(msg) : msg;
    uint64_t reg = crcRegister(m.width, m.poly, m.init, fed);
    if (m.refout) reg = reflectBits(reg, m.width);
    return (reg ^ m.xorout) & crcMask(m.width);
}

inline uint64_t crcBytes(const CrcModel& m, const std::vector<uint8_t>& data) { return crcCompute(m, fromBytes(data)); }

// ---- the catalogue (data) -----------------------------------------------------------------------

struct CatalogueEntry {
    CrcModel model;
    uint64_t check;  // CRC of the nine ASCII digits "123456789"
};

inline const std::vector<CatalogueEntry>& crcCatalogue() {
    static const std::vector<CatalogueEntry> t = {
        {{8, 0x07, 0x00, 0x00, false, false, "CRC-8/SMBUS"}, 0xf4},
        {{8, 0x31, 0x00, 0x00, true, true, "CRC-8/MAXIM-DOW"}, 0xa1},
        {{8, 0x07, 0xff, 0x00, true, true, "CRC-8/ROHC"}, 0xd0},
        {{16, 0x8005, 0x0000, 0x0000, true, true, "CRC-16/ARC"}, 0xbb3d},
        {{16, 0x8005, 0xffff, 0x0000, true, true, "CRC-16/MODBUS"}, 0x4b37},
        {{16, 0x8005, 0xffff, 0xffff, true, true, "CRC-16/USB"}, 0xb4c8},
        {{16, 0x1021, 0x0000, 0x0000, true, true, "CRC-16/KERMIT"}, 0x2189},
        {{16, 0x1021, 0x0000, 0x0000, false, false, "CRC-16/XMODEM"}, 0x31c3},
        {{16, 0x1021, 0xffff, 0x0000, false, false, "CRC-16/IBM-3740 (CCITT-FALSE)"}, 0x29b1},
        {{16, 0x1021, 0xffff, 0xffff, false, false, "CRC-16/GENIBUS"}, 0xd64e},
        {{16, 0x1021, 0xffff, 0xffff, true, true, "CRC-16/IBM-SDLC (X-25)"}, 0x906e},
        {{16, 0x1021, 0xffff, 0x0000, true, true, "CRC-16/MCRF4XX"}, 0x6f91},
        {{16, 0x1021, 0x89ec, 0x0000, true, true, "CRC-16/TMS37157"}, 0x26b1},
        {{16, 0x3d65, 0x0000, 0xffff, true, true, "CRC-16/DNP"}, 0xea82},
        {{24, 0x864cfb, 0xb704ce, 0x000000, false, false, "CRC-24/OPENPGP"}, 0x21cf02},
        {{32, 0x04c11db7, 0xffffffff, 0xffffffff, true, true, "CRC-32/ISO-HDLC"}, 0xcbf43926},
        {{32, 0x1edc6f41, 0xffffffff, 0xffffffff, true, true, "CRC-32/ISCSI"}, 0xe3069283},
        {{32, 0x04c11db7, 0xffffffff, 0xffffffff, false, false, "CRC-32/BZIP2"}, 0xfc891918},
    };
    return t;
}

// ---- GF(2) polynomial arithmetic (for the finder) --------------------------------------------------

namespace gf2 {
using Poly = std::vector<uint64_t>;  // little-endian words, coefficient i = bit i

inline int degree(const Poly& p) {
    for (size_t i = p.size(); i-- > 0;)
        if (p[i]) {
            int d = 63;
            while (!((p[i] >> d) & 1u)) --d;
            return static_cast<int>(i * 64) + d;
        }
    return -1;
}
inline bool coeff(const Poly& p, size_t i) { return i / 64 < p.size() && ((p[i / 64] >> (i % 64)) & 1u); }
inline void flip(Poly& p, size_t i) {
    if (i / 64 >= p.size()) p.resize(i / 64 + 1, 0);
    p[i / 64] ^= 1ull << (i % 64);
}
// a ^= b << shift
inline void xorShifted(Poly& a, const Poly& b, size_t shift) {
    const size_t ws = shift / 64, bs = shift % 64;
    if (a.size() < b.size() + ws + 1) a.resize(b.size() + ws + 1, 0);
    for (size_t i = 0; i < b.size(); ++i) {
        a[i + ws] ^= b[i] << bs;
        if (bs) a[i + ws + 1] ^= b[i] >> (64 - bs);
    }
}
inline void trim(Poly& p) {
    while (!p.empty() && p.back() == 0) p.pop_back();
}
// r = a mod b (b nonzero); q (optional) receives the quotient.
inline Poly mod(Poly a, const Poly& b, Poly* q = nullptr) {
    const int db = degree(b);
    if (q) q->clear();
    if (db < 0) return a;
    for (int da = degree(a); da >= db; da = degree(a)) {
        xorShifted(a, b, static_cast<size_t>(da - db));
        if (q) flip(*q, static_cast<size_t>(da - db));
    }
    trim(a);
    return a;
}
inline Poly gcd(Poly a, Poly b) {
    trim(a);
    trim(b);
    while (!b.empty()) {
        Poly r = mod(a, b);
        a = std::move(b);
        b = std::move(r);
    }
    return a;
}
// Divide out every factor x (a generator is odd, so x never belongs to it: when all the sample
// differences happen to share low zero bits, those are common factors of the gcd, not of g).
inline Poly stripX(Poly p) {
    trim(p);
    if (p.empty()) return p;
    size_t z = 0;
    while (!coeff(p, z)) ++z;
    if (z == 0) return p;
    Poly r(p.size(), 0);
    for (size_t i = z; i < p.size() * 64; ++i)
        if (coeff(p, i)) r[(i - z) / 64] |= 1ull << ((i - z) % 64);
    trim(r);
    return r;
}
// A bit sequence (first bit = highest power) as a polynomial.
inline Poly fromBitSeq(const Bits& s) {
    Poly p((s.size() + 63) / 64, 0);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i]) p[(s.size() - 1 - i) / 64] |= 1ull << ((s.size() - 1 - i) % 64);
    return p;
}
}  // namespace gf2

// ---- the CRC finder --------------------------------------------------------------------------------

enum class ByteOrder : uint8_t { Big, Little };

struct CrcHit {
    CrcModel model;            // width, poly, init, xorout, refin, refout (name set when a catalogue entry matched)
    bool catalogueMatch = false;
    ByteOrder order = ByteOrder::Big;  // how the check value's bytes are laid out in the frame
    size_t headBytes = 0;      // bytes at the start of the frame that the CRC does not cover
    size_t tailBytes = 0;      // bytes after the check value
    bool derived = false;      // polynomial derived algebraically (not only matched against the catalogue)
    size_t candidates = 1;     // how many (polynomial) candidates fit equally well for this width/reflection/layout
    unsigned freeDims = 0;     // dimensions of (init, xorout) the frames cannot pin down (0 = unique)
    double coincidence = 0.0;  // rough chance that random frames would produce a hit like this one somewhere in the layouts tried
    std::string note;          // plain-language honesty line
};

struct CrcFindOptions {
    std::vector<unsigned> widths = {8, 16, 24, 32};
    size_t maxHeadBytes = 4;
    size_t maxTailBytes = 2;
    size_t maxHits = 32;
    size_t maxFrameBytes = 1024;  // longer frames are ignored (resource limit)
    size_t maxCofactorDegree = 16;
    std::vector<std::string>* trace = nullptr;  // optional: one line per derivation attempt (why a layout did or did not give a hit)
};

namespace detail {

inline uint64_t readValue(const std::vector<uint8_t>& f, size_t pos, unsigned bytes, ByteOrder o) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        const uint64_t b = f[pos + (o == ByteOrder::Big ? i : bytes - 1 - i)];
        v = (v << 8) | b;
    }
    return v;
}

// Gaussian elimination over GF(2) on rows of up to 128 unknown bits + 1 right-hand-side column. Each
// row is (lo, hi, rhs): unknown bits 0..63 in lo and 64..127 in hi. Returns false if inconsistent;
// otherwise `rank`, and a solution with every free variable zero.
struct Row {
    uint64_t lo = 0, hi = 0;
    uint64_t rhs = 0;
};
inline bool solveGf2(std::vector<Row> rows, unsigned unknowns, unsigned* rank, uint64_t* solLo, uint64_t* solHi) {
    std::vector<int> pivCol;
    unsigned r = 0;
    for (unsigned col = 0; col < unknowns; ++col) {
        const auto bit = [&](const Row& x) { return col < 64 ? ((x.lo >> col) & 1u) : ((x.hi >> (col - 64)) & 1u); };
        size_t p = r;
        while (p < rows.size() && !bit(rows[p])) ++p;
        if (p == rows.size()) continue;
        std::swap(rows[r], rows[p]);
        for (size_t i = 0; i < rows.size(); ++i)
            if (i != r && bit(rows[i])) {
                rows[i].lo ^= rows[r].lo;
                rows[i].hi ^= rows[r].hi;
                rows[i].rhs ^= rows[r].rhs;
            }
        pivCol.push_back(static_cast<int>(col));
        ++r;
    }
    for (size_t i = r; i < rows.size(); ++i)
        if (rows[i].rhs & 1u) return false;
    uint64_t lo = 0, hi = 0;
    for (unsigned i = 0; i < r; ++i)
        if (rows[i].rhs & 1u) {
            if (pivCol[i] < 64) lo |= 1ull << pivCol[i];
            else hi |= 1ull << (pivCol[i] - 64);
        }
    *rank = r;
    *solLo = lo;
    *solHi = hi;
    return true;
}

}  // namespace detail

// Find the CRC definition from sample frames (each a byte vector holding head + data + check value +
// tail). Needs at least one frame; two or more frames of one length let the polynomial be DERIVED;
// with fewer the finder can only try the catalogue. Every hit reproduces the check value of EVERY frame.
inline std::vector<CrcHit> findCrc(const std::vector<std::vector<uint8_t>>& framesIn, const CrcFindOptions& opt = {}) {
    std::vector<CrcHit> hits;
    std::vector<std::vector<uint8_t>> frames;
    for (const auto& f : framesIn)
        if (!f.empty() && f.size() <= opt.maxFrameBytes) frames.push_back(f);
    if (frames.empty()) return hits;

    auto addHit = [&](CrcHit h) {
        for (const auto& e : hits)
            if (e.model.width == h.model.width && e.model.poly == h.model.poly && e.model.init == h.model.init &&
                e.model.xorout == h.model.xorout && e.model.refin == h.model.refin && e.model.refout == h.model.refout &&
                e.order == h.order && e.headBytes == h.headBytes && e.tailBytes == h.tailBytes)
                return;
        if (hits.size() < opt.maxHits) hits.push_back(std::move(h));
    };

    for (unsigned w : opt.widths) {
        if (w < 8 || w > 64 || w % 8) continue;
        const unsigned wb = w / 8;
        for (size_t head = 0; head <= opt.maxHeadBytes; ++head)
            for (size_t tail = 0; tail <= opt.maxTailBytes; ++tail) {
                // every frame must be long enough to hold head + at least one data byte + check + tail
                bool fits = true;
                for (const auto& f : frames) fits &= f.size() >= head + 1 + wb + tail;
                if (!fits) continue;
                for (ByteOrder order : {ByteOrder::Big, ByteOrder::Little}) {
                    // data bits and check values per frame
                    std::vector<Bits> data(frames.size());
                    std::vector<uint64_t> cv(frames.size());
                    for (size_t i = 0; i < frames.size(); ++i) {
                        const auto& f = frames[i];
                        const size_t cpos = f.size() - tail - wb;
                        data[i] = fromBytes(std::vector<uint8_t>(f.begin() + static_cast<std::ptrdiff_t>(head), f.begin() + static_cast<std::ptrdiff_t>(cpos)));
                        cv[i] = detail::readValue(f, cpos, wb, order);
                    }
                    // 1. the catalogue: every entry of this width that reproduces every frame
                    for (const auto& e : crcCatalogue()) {
                        if (e.model.width != w) continue;
                        bool all = true;
                        for (size_t i = 0; i < frames.size() && all; ++i) {
                            bool ok = true;
                            all &= crcCompute(e.model, data[i], &ok) == cv[i] && ok;
                        }
                        if (!all) continue;
                        CrcHit h;
                        h.model = e.model;
                        h.catalogueMatch = true;
                        h.order = order;
                        h.headBytes = head;
                        h.tailBytes = tail;
                        h.coincidence = std::ldexp(1.0, -static_cast<int>(w * frames.size())) * static_cast<double>(crcCatalogue().size());
                        h.note = frames.size() == 1 ? "one frame only: a coincidence is possible (chance about 2^-" + std::to_string(w) + " per definition tried)"
                                                    : "reproduces the check value of every frame";
                        addHit(std::move(h));
                    }
                    // 2. derive the polynomial from groups of equal-length frames
                    std::vector<std::vector<size_t>> groups;
                    for (size_t i = 0; i < frames.size(); ++i) {
                        bool placed = false;
                        for (auto& g : groups)
                            if (frames[g[0]].size() == frames[i].size()) { g.push_back(i); placed = true; break; }
                        if (!placed) groups.push_back({i});
                    }
                    bool anyPair = false;
                    for (const auto& g : groups) anyPair |= g.size() >= 2;
                    if (!anyPair) continue;
                    // Deriving needs at least TWO independent differences (three frames of one length, or two
                    // groups): one difference is a single polynomial with any number of divisors.
                    size_t diffCount = 0;
                    for (const auto& g : groups) diffCount += g.size() - 1;
                    if (diffCount < 2) continue;
                    for (int refin = 0; refin < 2; ++refin)
                        for (int refout = 0; refout < 2; ++refout) {
                            if (refin && data[0].size() % 8) continue;
                            // R(c) per frame, and the fed data per frame
                            std::vector<Bits> fed(frames.size());
                            std::vector<uint64_t> rc(frames.size());
                            for (size_t i = 0; i < frames.size(); ++i) {
                                fed[i] = refin ? reverseBitsInBytes(data[i]) : data[i];
                                rc[i] = refout ? reflectBits(cv[i], w) : cv[i];
                            }
                            // gcd of E over all pairs-with-the-first of each group
                            gf2::Poly g;
                            bool haveG = false;
                            for (const auto& grp : groups) {
                                for (size_t k = 1; k < grp.size(); ++k) {
                                    const size_t a = grp[0], b = grp[k];
                                    Bits seq(fed[a].size());
                                    for (size_t j = 0; j < seq.size(); ++j) seq[j] = static_cast<uint8_t>(fed[a][j] ^ fed[b][j]);
                                    const uint64_t d = rc[a] ^ rc[b];
                                    for (int j = static_cast<int>(w) - 1; j >= 0; --j) seq.push_back(static_cast<uint8_t>((d >> j) & 1u));
                                    gf2::Poly E = gf2::fromBitSeq(seq);
                                    g = haveG ? gf2::gcd(g, E) : E;
                                    gf2::trim(g);
                                    haveG = true;
                                }
                            }
                            if (!haveG) continue;
                            g = gf2::stripX(g);
                            const int dg = gf2::degree(g);
                            if (opt.trace && opt.trace->size() < 400)
                                opt.trace->push_back("w=" + std::to_string(w) + " refin=" + std::to_string(refin) + " refout=" + std::to_string(refout) +
                                                     " head=" + std::to_string(head) + " tail=" + std::to_string(tail) + (order == ByteOrder::Big ? " big" : " little") +
                                                     ": gcd degree " + std::to_string(dg));
                            if (dg < static_cast<int>(w)) continue;  // no degree-w generator divides all differences
                            std::vector<gf2::Poly> cands;
                            if (dg == static_cast<int>(w)) {
                                if (gf2::coeff(g, 0)) cands.push_back(g);  // a CRC polynomial is odd
                            } else if (dg - static_cast<int>(w) <= static_cast<int>(opt.maxCofactorDegree)) {
                                const unsigned k = static_cast<unsigned>(dg) - w;
                                for (uint64_t qv = (1ull << k) | 1ull; qv < (2ull << k); qv += 2) {
                                    gf2::Poly q{qv};
                                    gf2::Poly quo;
                                    if (!gf2::mod(g, q, &quo).empty()) continue;
                                    // quotient must have a constant term (a CRC polynomial is odd)
                                    if (!gf2::coeff(quo, 0)) continue;
                                    cands.push_back(quo);
                                    if (cands.size() >= 64) break;
                                }
                            }
                            for (const auto& cp : cands) {
                                CrcModel m;
                                m.width = w;
                                m.poly = cp.empty() ? 0 : (cp[0] & crcMask(w));
                                m.refin = refin != 0;
                                m.refout = refout != 0;
                                // 3. init and final XOR: R(c) ^ crc0(fed) = A_n(init) ^ X'  (X' = R(xorout))
                                std::vector<detail::Row> rows;
                                for (size_t i = 0; i < frames.size(); ++i) {
                                    const uint64_t c0 = crcRegister(w, m.poly, 0, fed[i]);
                                    const uint64_t target = rc[i] ^ c0;
                                    // column for init bit j: the register after feeding n zero bits from e_j
                                    const Bits zeros(fed[i].size(), 0);
                                    std::vector<uint64_t> col(w);
                                    for (unsigned j = 0; j < w; ++j) col[j] = crcRegister(w, m.poly, 1ull << j, zeros);
                                    for (unsigned bit = 0; bit < w; ++bit) {
                                        detail::Row r;
                                        for (unsigned j = 0; j < w; ++j)
                                            if ((col[j] >> bit) & 1u) { if (j < 64) r.lo |= 1ull << j; else r.hi |= 1ull << (j - 64); }
                                        // X' bit `bit` is unknown number w + bit
                                        if (w + bit < 64) r.lo |= 1ull << (w + bit);
                                        else r.hi |= 1ull << (w + bit - 64);
                                        r.rhs = (target >> bit) & 1u;
                                        rows.push_back(r);
                                    }
                                }
                                unsigned rank = 0;
                                uint64_t sl = 0, sh = 0;
                                if (2 * w > 128 || !detail::solveGf2(rows, 2 * w, &rank, &sl, &sh)) continue;
                                // Prefer the conventional constants when the frames allow them
                                const uint64_t ones = crcMask(w);
                                auto consistent = [&](uint64_t init, uint64_t xr) {
                                    CrcModel t = m;
                                    t.init = init;
                                    t.xorout = refout ? reflectBits(xr, w) : xr;
                                    for (size_t i = 0; i < frames.size(); ++i) {
                                        bool ok = true;
                                        if (crcCompute(t, data[i], &ok) != cv[i] || !ok) return false;
                                    }
                                    return true;
                                };
                                uint64_t init = 0, xr = 0;
                                bool chosen = false;
                                for (uint64_t i0 : {0ull, ones})
                                    for (uint64_t x0 : {0ull, ones})
                                        if (!chosen && consistent(i0, x0)) { init = i0; xr = x0; chosen = true; }
                                if (!chosen) {
                                    init = sl & ones;
                                    xr = (w < 64) ? (((sl >> w) | (sh << (64 - w))) & ones) : sh;
                                    if (!consistent(init, xr)) continue;
                                }
                                m.init = init;
                                m.xorout = refout ? reflectBits(xr, w) : xr;
                                // a catalogue entry that matches this polynomial and reproduces every frame is named
                                CrcHit h;
                                h.model = m;
                                h.order = order;
                                h.headBytes = head;
                                h.tailBytes = tail;
                                h.derived = true;
                                h.candidates = cands.size();
                                h.freeDims = 2 * w - rank;
                                {
                                    size_t diffs = 0;
                                    for (const auto& grp : groups) diffs += grp.size() - 1;
                                    const double layouts = static_cast<double>((opt.maxHeadBytes + 1) * (opt.maxTailBytes + 1) * 8);
                                    h.coincidence = layouts * std::ldexp(1.0, -static_cast<int>(w * (diffs > 1 ? diffs - 1 : 1)));
                                }
                                for (const auto& e : crcCatalogue()) {
                                    if (e.model.width != w || e.model.poly != m.poly || e.model.refin != m.refin || e.model.refout != m.refout) continue;
                                    bool all = true;
                                    for (size_t i = 0; i < frames.size() && all; ++i) {
                                        bool ok = true;
                                        all &= crcCompute(e.model, data[i], &ok) == cv[i] && ok;
                                    }
                                    if (all) { h.model = e.model; h.catalogueMatch = true; break; }
                                }
                                std::string n;
                                if (cands.size() > 1) n += std::to_string(cands.size()) + " polynomials divide every difference: the frames do not pin the polynomial down. ";
                                if (h.freeDims > 0)
                                    n += "init and final XOR cannot be told apart from these frames (" + std::to_string(h.freeDims) +
                                         " free dimensions): the values shown are one equivalent choice; frames of two or more different lengths would settle it. ";
                                if (n.empty()) n = "polynomial, init and final XOR are all determined by the frames";
                                h.note = n;
                                addHit(std::move(h));
                            }
                        }
                }
            }
    }
    // Catalogue matches first, then derived ones with fewest candidates.
    std::stable_sort(hits.begin(), hits.end(), [](const CrcHit& a, const CrcHit& b) {
        if (a.catalogueMatch != b.catalogueMatch) return a.catalogueMatch;
        if (a.candidates != b.candidates) return a.candidates < b.candidates;
        return a.freeDims < b.freeDims;
    });
    return hits;
}

// ---- additive / XOR / Fletcher checksums -----------------------------------------------------------

enum class SumKind : uint8_t {
    Sum8,         // sum of the bytes mod 256
    Sum8Neg,      // two's complement of the sum (the frame adds up to zero)
    Xor8,         // XOR of the bytes
    Xor8Inv,      // XOR of the bytes, started from 0xFF
    Sum16Bytes,   // sum of the bytes mod 65536 (2-byte check value)
    NibbleSum,    // sum of the 4-bit nibbles mod 256 (1-byte check value)
    Fletcher16,   // Fletcher-16: two running sums mod 255
    Fletcher16_256,  // the same mod 256
    Internet16,   // ones'-complement sum of 16-bit big-endian words, inverted (RFC 1071 form)
};

inline const char* sumName(SumKind k) {
    switch (k) {
        case SumKind::Sum8: return "sum of bytes mod 256";
        case SumKind::Sum8Neg: return "two's-complement sum of bytes";
        case SumKind::Xor8: return "XOR of bytes";
        case SumKind::Xor8Inv: return "XOR of bytes from 0xFF";
        case SumKind::Sum16Bytes: return "16-bit sum of bytes";
        case SumKind::NibbleSum: return "sum of nibbles";
        case SumKind::Fletcher16: return "Fletcher-16 (mod 255)";
        case SumKind::Fletcher16_256: return "Fletcher-16 (mod 256)";
        case SumKind::Internet16: return "Internet checksum (ones'-complement 16-bit)";
    }
    return "?";
}
inline unsigned sumBytes(SumKind k) {
    switch (k) {
        case SumKind::Sum16Bytes: case SumKind::Fletcher16: case SumKind::Fletcher16_256: case SumKind::Internet16: return 2;
        default: return 1;
    }
}

// The checksum of `data` under `k`. For Fletcher the result is (sum2 << 8) | sum1.
inline uint32_t sumCompute(SumKind k, const uint8_t* d, size_t n) {
    switch (k) {
        case SumKind::Sum8: { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return s & 0xFF; }
        case SumKind::Sum8Neg: { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return (0x100 - (s & 0xFF)) & 0xFF; }
        case SumKind::Xor8: { unsigned s = 0; for (size_t i = 0; i < n; ++i) s ^= d[i]; return s; }
        case SumKind::Xor8Inv: { unsigned s = 0xFF; for (size_t i = 0; i < n; ++i) s ^= d[i]; return s; }
        case SumKind::Sum16Bytes: { uint32_t s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return s & 0xFFFF; }
        case SumKind::NibbleSum: { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += (d[i] >> 4) + (d[i] & 15u); return s & 0xFF; }
        case SumKind::Fletcher16:
        case SumKind::Fletcher16_256: {
            const unsigned m = k == SumKind::Fletcher16 ? 255 : 256;
            unsigned a = 0, b = 0;
            for (size_t i = 0; i < n; ++i) { a = (a + d[i]) % m; b = (b + a) % m; }
            return (b << 8) | a;
        }
        case SumKind::Internet16: {
            uint32_t s = 0;
            for (size_t i = 0; i < n; i += 2) s += (static_cast<uint32_t>(d[i]) << 8) | (i + 1 < n ? d[i + 1] : 0u);
            while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
            return (~s) & 0xFFFF;
        }
    }
    return 0;
}

struct SumHit {
    SumKind kind = SumKind::Sum8;
    ByteOrder order = ByteOrder::Big;  // for the two-byte kinds
    size_t headBytes = 0, tailBytes = 0;
};

// Every (kind, byte order, head, tail) that reproduces the check value of every frame.
inline std::vector<SumHit> findChecksums(const std::vector<std::vector<uint8_t>>& frames, size_t maxHead = 4, size_t maxTail = 2) {
    std::vector<SumHit> out;
    if (frames.empty()) return out;
    for (int ki = 0; ki <= static_cast<int>(SumKind::Internet16); ++ki) {
        const SumKind k = static_cast<SumKind>(ki);
        const unsigned nb = sumBytes(k);
        for (size_t head = 0; head <= maxHead; ++head)
            for (size_t tail = 0; tail <= maxTail; ++tail)
                for (ByteOrder o : {ByteOrder::Big, ByteOrder::Little}) {
                    if (nb == 1 && o == ByteOrder::Little) continue;
                    bool all = true;
                    for (const auto& f : frames) {
                        if (f.size() < head + 1 + nb + tail) { all = false; break; }
                        const size_t cpos = f.size() - tail - nb;
                        const uint32_t want = static_cast<uint32_t>(detail::readValue(f, cpos, nb, o));
                        const uint32_t got = sumCompute(k, f.data() + head, cpos - head);
                        if (got != want) { all = false; break; }  // Fletcher: the 16-bit value is (sum2 << 8) | sum1, read in `o` order
                    }
                    if (all) out.push_back({k, o, head, tail});
                }
    }
    return out;
}

}  // namespace cascade::bits
