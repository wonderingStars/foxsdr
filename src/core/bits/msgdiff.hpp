// BIT-VIEW LIBRARY, part 6: message diffing.
//
// diffBits aligns two bit strings with a global (Needleman-Wunsch) edit alignment - match +1, mismatch
// -1, gap -2, so a substitution is preferred to a gap and an inserted bit is found when the
// remaining bits line up - and returns the aligned rows with gaps, a markup row and the runs where
// they differ. Inputs over `maxBits` per side are refused (the table is n * m cells).
//
// diffFrames compares many frames column by column (which bit positions vary and how often).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/bits/bits.hpp"

namespace cascade::bits {

enum class DiffOp : uint8_t { Match, Mismatch, OnlyA, OnlyB };  // OnlyA: a bit in A with a gap in B

struct DiffRun {
    size_t aStart = 0, bStart = 0;  // first bit of the run in each string
    size_t aLen = 0, bLen = 0;      // bits consumed from each string by the run
};

struct BitDiff {
    bool ok = false;               // false: an input was over the limit
    std::vector<DiffOp> ops;
    std::string rowA, rowB, marks;  // aligned text rows: '0'/'1' or '-' for a gap; marks '|' match, 'x' mismatch, ' ' gap
    size_t matches = 0, mismatches = 0, gaps = 0;
    std::vector<DiffRun> runs;      // maximal stretches that are not plain matches
};

inline BitDiff diffBits(const Bits& a, const Bits& b, bool allowGaps = true, size_t maxBits = 2048) {
    BitDiff d;
    if (a.size() > maxBits || b.size() > maxBits) return d;
    d.ok = true;
    const size_t n = a.size(), m = b.size();
    if (!allowGaps) {
        const size_t common = std::min(n, m);
        for (size_t i = 0; i < common; ++i) d.ops.push_back(a[i] == b[i] ? DiffOp::Match : DiffOp::Mismatch);
        for (size_t i = common; i < n; ++i) d.ops.push_back(DiffOp::OnlyA);
        for (size_t i = common; i < m; ++i) d.ops.push_back(DiffOp::OnlyB);
    } else {
        constexpr int kMatch = 1, kMis = -1, kGap = -2;
        std::vector<int> score((n + 1) * (m + 1));
        std::vector<uint8_t> from((n + 1) * (m + 1), 0);  // 0 diag, 1 up (OnlyA), 2 left (OnlyB)
        auto at = [&](size_t i, size_t j) -> int& { return score[i * (m + 1) + j]; };
        for (size_t i = 1; i <= n; ++i) { at(i, 0) = static_cast<int>(i) * kGap; from[i * (m + 1)] = 1; }
        for (size_t j = 1; j <= m; ++j) { at(0, j) = static_cast<int>(j) * kGap; from[j] = 2; }
        for (size_t i = 1; i <= n; ++i)
            for (size_t j = 1; j <= m; ++j) {
                const int diag = at(i - 1, j - 1) + (a[i - 1] == b[j - 1] ? kMatch : kMis);
                const int up = at(i - 1, j) + kGap, left = at(i, j - 1) + kGap;
                int best = diag;
                uint8_t f = 0;
                if (up > best) { best = up; f = 1; }
                if (left > best) { best = left; f = 2; }
                at(i, j) = best;
                from[i * (m + 1) + j] = f;
            }
        size_t i = n, j = m;
        std::vector<DiffOp> rev;
        while (i > 0 || j > 0) {
            const uint8_t f = from[i * (m + 1) + j];
            if (i > 0 && j > 0 && f == 0) { rev.push_back(a[i - 1] == b[j - 1] ? DiffOp::Match : DiffOp::Mismatch); --i; --j; }
            else if (i > 0 && (f == 1 || j == 0)) { rev.push_back(DiffOp::OnlyA); --i; }
            else { rev.push_back(DiffOp::OnlyB); --j; }
        }
        d.ops.assign(rev.rbegin(), rev.rend());
    }
    size_t ia = 0, ib = 0;
    DiffRun cur;
    bool inRun = false;
    for (DiffOp op : d.ops) {
        char ca = '-', cb = '-', mk = ' ';
        switch (op) {
            case DiffOp::Match: ca = static_cast<char>('0' + a[ia]); cb = static_cast<char>('0' + b[ib]); mk = '|'; ++d.matches; break;
            case DiffOp::Mismatch: ca = static_cast<char>('0' + a[ia]); cb = static_cast<char>('0' + b[ib]); mk = 'x'; ++d.mismatches; break;
            case DiffOp::OnlyA: ca = static_cast<char>('0' + a[ia]); ++d.gaps; break;
            case DiffOp::OnlyB: cb = static_cast<char>('0' + b[ib]); ++d.gaps; break;
        }
        d.rowA.push_back(ca);
        d.rowB.push_back(cb);
        d.marks.push_back(mk);
        if (op != DiffOp::Match) {
            if (!inRun) { cur = DiffRun{ia, ib, 0, 0}; inRun = true; }
            if (op != DiffOp::OnlyB) ++cur.aLen;
            if (op != DiffOp::OnlyA) ++cur.bLen;
        } else if (inRun) {
            d.runs.push_back(cur);
            inRun = false;
        }
        if (op != DiffOp::OnlyB) ++ia;
        if (op != DiffOp::OnlyA) ++ib;
    }
    if (inRun) d.runs.push_back(cur);
    return d;
}

struct ColumnStat {
    size_t ones = 0;
    size_t count = 0;
    bool constant() const { return count > 0 && (ones == 0 || ones == count); }
};

// Per-column statistics of frames of any length: column i counts the frames that are at least i+1 long.
inline std::vector<ColumnStat> diffFrames(const std::vector<Bits>& frames, size_t maxColumns = 8192) {
    size_t cols = 0;
    for (const auto& f : frames) cols = std::max(cols, f.size());
    cols = std::min(cols, maxColumns);
    std::vector<ColumnStat> st(cols);
    for (const auto& f : frames)
        for (size_t i = 0; i < f.size() && i < cols; ++i) {
            ++st[i].count;
            st[i].ones += f[i] & 1u;
        }
    return st;
}

// Hex dump rows of two byte strings with the differing bytes marked by '^' underneath (for the byte view).
inline std::string diffBytesText(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    static const char* hex = "0123456789ABCDEF";
    std::string l1, l2, mk;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const bool ha = i < a.size(), hb = i < b.size();
        auto put = [&](std::string& s, bool has, uint8_t v) {
            if (has) { s.push_back(hex[v >> 4]); s.push_back(hex[v & 15]); }
            else s += "--";
            s.push_back(' ');
        };
        put(l1, ha, ha ? a[i] : 0);
        put(l2, hb, hb ? b[i] : 0);
        mk += (ha && hb && a[i] == b[i]) ? "   " : "^^ ";
    }
    return l1 + "\n" + l2 + "\n" + mk + "\n";
}

}  // namespace cascade::bits
