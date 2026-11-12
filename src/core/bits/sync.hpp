// BIT-VIEW LIBRARY, part 4: preamble and sync-word search, and framing.
//
// findPattern slides a pattern along a bit stream and reports every offset whose Hamming distance
// to the pattern is at most `maxDist` (overlapping hits inside the tolerance are collapsed to the
// best one within a pattern length). Framing cuts a stream into frames by a length field, by a
// delimiter (a flag, optionally with HDLC-style bit stuffing) or by fixed size.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <vector>

#include "core/bits/bits.hpp"

namespace cascade::bits {

struct PatternHit {
    size_t pos = 0;       // offset of the first pattern bit
    size_t distance = 0;  // bits that differ
    bool inverted = false;
};

// Resource limit: patterns longer than 4096 bits, or empty, find nothing.
inline std::vector<PatternHit> findPattern(const Bits& stream, const Bits& pattern, size_t maxDist = 0, bool alsoInverted = false,
                                           size_t maxHits = 4096) {
    std::vector<PatternHit> raw;
    const size_t m = pattern.size();
    if (m == 0 || m > 4096 || stream.size() < m) return raw;
    for (size_t i = 0; i + m <= stream.size(); ++i) {
        size_t d = 0, di = 0;
        for (size_t k = 0; k < m; ++k) {
            const unsigned x = (stream[i + k] ^ pattern[k]) & 1u;
            d += x;
            di += x ^ 1u;
            if (d > maxDist && (!alsoInverted || di > maxDist)) break;
        }
        if (d <= maxDist) raw.push_back({i, d, false});
        else if (alsoInverted && di <= maxDist) raw.push_back({i, di, true});
    }
    // Collapse neighbours: inside any window of m bits keep the hit with the smallest distance
    // (the earliest on a tie).
    std::vector<PatternHit> out;
    for (const auto& h : raw) {
        if (!out.empty() && h.pos < out.back().pos + m) {
            if (h.distance < out.back().distance) out.back() = h;
        } else {
            out.push_back(h);
        }
        if (out.size() >= maxHits) break;
    }
    return out;
}

// Preamble: the longest run at the start of `bits` (from `from`) that alternates 1010... or
// 0101... or is constant; returns its length and the kind. Used to see where a preamble ends and
// the sync word begins. `minLen` below which 0 is returned.
struct PreambleRun {
    size_t length = 0;
    enum Kind { None, Alternating, Constant } kind = None;
};
inline PreambleRun measurePreamble(const Bits& b, size_t from = 0, size_t minLen = 8) {
    PreambleRun r;
    if (from >= b.size()) return r;
    size_t alt = 1;
    while (from + alt < b.size() && b[from + alt] != b[from + alt - 1]) ++alt;
    size_t con = 1;
    while (from + con < b.size() && b[from + con] == b[from]) ++con;
    if (alt >= con && alt >= minLen) { r.length = alt; r.kind = PreambleRun::Alternating; }
    else if (con > alt && con >= minLen) { r.length = con; r.kind = PreambleRun::Constant; }
    return r;
}

struct Frame {
    size_t start = 0;  // bit offset of the first payload bit (after the sync word)
    Bits bits;
    size_t syncDistance = 0;
};

// Fixed size: after each sync hit take `frameBits` bits (a short tail at the end of the stream is
// dropped unless `keepShort`).
inline std::vector<Frame> frameFixed(const Bits& stream, const std::vector<PatternHit>& syncs, size_t syncLen, size_t frameBits,
                                     bool keepShort = false) {
    std::vector<Frame> out;
    for (const auto& h : syncs) {
        const size_t s = h.pos + syncLen;
        if (s > stream.size()) continue;
        Frame f;
        f.start = s;
        f.syncDistance = h.distance;
        f.bits = slice(stream, s, frameBits);
        if (f.bits.size() < frameBits && !keepShort) continue;
        out.push_back(std::move(f));
    }
    return out;
}

// By a length field: `lenPos`/`lenBits` locate it inside the frame (counted from the first payload
// bit), its value times `unitBits` plus `adjustBits` (which may be negative) is the frame length in
// bits INCLUDING the length field itself. A length that is zero, beyond `maxBits` or past the end
// of the stream drops the frame (counted in `rejected`).
struct LengthFraming {
    size_t lenPos = 0, lenBits = 8;
    size_t unitBits = 8;
    long adjustBits = 0;
    size_t maxBits = 4096;
    bool msbFirst = true;
};
inline std::vector<Frame> frameByLength(const Bits& stream, const std::vector<PatternHit>& syncs, size_t syncLen,
                                        const LengthFraming& lf, size_t* rejected = nullptr) {
    std::vector<Frame> out;
    size_t rej = 0;
    for (const auto& h : syncs) {
        const size_t s = h.pos + syncLen;
        if (lf.lenBits == 0 || lf.lenBits > 32 || s + lf.lenPos + lf.lenBits > stream.size()) { ++rej; continue; }
        const uint64_t v = getField(stream, s + lf.lenPos, lf.lenBits, lf.msbFirst);
        const long long total = static_cast<long long>(v) * static_cast<long long>(lf.unitBits) + lf.adjustBits;
        if (total <= 0 || static_cast<size_t>(total) > lf.maxBits || s + static_cast<size_t>(total) > stream.size()) { ++rej; continue; }
        Frame f;
        f.start = s;
        f.syncDistance = h.distance;
        f.bits = slice(stream, s, static_cast<size_t>(total));
        out.push_back(std::move(f));
    }
    if (rejected) *rejected = rej;
    return out;
}

// By delimiter: frames are the stretches between occurrences of `flag` (e.g. 01111110). With
// `unstuff` a 0 that follows five 1s inside a frame is removed (HDLC bit stuffing); a run of six or
// more 1s that is not the flag aborts the frame (counted in `aborted`). Empty frames and frames
// over `maxBits` are dropped.
inline std::vector<Frame> frameByFlag(const Bits& stream, const Bits& flag, bool unstuff, size_t maxBits = 4096, size_t* aborted = nullptr) {
    std::vector<Frame> out;
    size_t abortedN = 0;
    const auto hits = findPattern(stream, flag, 0, false);
    for (size_t i = 0; i + 1 < hits.size(); ++i) {
        const size_t a = hits[i].pos + flag.size(), b = hits[i + 1].pos;
        if (b <= a) continue;
        Bits raw = slice(stream, a, b - a), body;
        bool bad = false;
        if (unstuff) {
            unsigned ones = 0;
            for (size_t k = 0; k < raw.size(); ++k) {
                if (ones == 5) {
                    if (raw[k] == 0) { ones = 0; continue; }  // stuffed bit
                    bad = true;
                    break;
                }
                body.push_back(raw[k]);
                ones = raw[k] ? ones + 1 : 0;
            }
        } else {
            body = std::move(raw);
        }
        if (bad) { ++abortedN; continue; }
        if (body.empty() || body.size() > maxBits) continue;
        Frame f;
        f.start = a;
        f.bits = std::move(body);
        out.push_back(std::move(f));
    }
    if (aborted) *aborted = abortedN;
    return out;
}

// The matching encoder (for the tests): flag, body with a 0 inserted after every five 1s, flag.
inline Bits stuffFrame(const Bits& body, const Bits& flag) {
    Bits out(flag);
    unsigned ones = 0;
    for (uint8_t b : body) {
        out.push_back(b);
        ones = b ? ones + 1 : 0;
        if (ones == 5) { out.push_back(0); ones = 0; }
    }
    out.insert(out.end(), flag.begin(), flag.end());
    return out;
}

}  // namespace cascade::bits
