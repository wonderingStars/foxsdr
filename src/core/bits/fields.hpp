// BIT-VIEW LIBRARY, part 7: automatic field inference from a set of frames.
//
// Input: frames (bit strings) in the ORDER they were received, which is what makes a counter
// visible. The frames are aligned at bit 0; only the columns present in every frame are examined
// (`minLength`), and a length field is looked for only when the frame lengths differ.
//
// What it reports, in bit order, covering [0, minLength):
//   Constant  - the same bits in every frame (preamble leftovers, an address of the device, a type)
//   Counter   - a field whose value rises by a constant step (mod 2^len) between consecutive frames
//   Length    - a field equal to the frame length (in bits or bytes) plus a constant, for frames of
//               differing length
//   Flags     - a short field with only a few distinct values
//   Data      - everything else that varies
// Checksum positions come from crc.hpp (findCrc / findChecksums), which `inferFields` calls when
// the frames are whole bytes and at least three are given; the field at the check position is
// then labelled Checksum with the name of the algorithm.
//
// The inference is a heuristic and says how sure it is: `evidence` is the number of frames the
// verdict rests on and `note` names the weakest point. Boundaries between two adjacent varying
// fields cannot be seen from the data alone: varying stretches are cut at byte boundaries.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/bits/bits.hpp"
#include "core/bits/crc.hpp"
#include "core/bits/msgdiff.hpp"

namespace cascade::bits {

enum class FieldKind : uint8_t { Constant, Counter, Length, Flags, Data, Checksum };

inline const char* fieldKindName(FieldKind k) {
    switch (k) {
        case FieldKind::Constant: return "constant";
        case FieldKind::Counter: return "counter";
        case FieldKind::Length: return "length";
        case FieldKind::Flags: return "flags";
        case FieldKind::Data: return "data";
        case FieldKind::Checksum: return "checksum";
    }
    return "?";
}

struct InferredField {
    size_t pos = 0, len = 0;    // in bits
    FieldKind kind = FieldKind::Data;
    uint64_t value = 0;         // Constant: the value; Counter: the step; Length: the constant added
    unsigned distinct = 0;      // distinct values seen (fields up to 32 bits)
    size_t evidence = 0;        // frames the verdict rests on
    bool lengthInBytes = false; // Length: counted in bytes
    std::string label;          // generated: "const_8", "counter_16", ...
    std::string note;
};

struct InferOptions {
    size_t maxFrames = 4096;
    size_t maxBits = 8192;
    double counterShare = 0.9;   // share of consecutive pairs that must show the same step
    unsigned flagsMaxDistinct = 4;
};

namespace detail {
inline bool wholeBytes(const std::vector<Bits>& fr) {
    for (const auto& f : fr) if (f.empty() || f.size() % 8) return false;
    return true;
}
}  // namespace detail

inline std::vector<InferredField> inferFields(const std::vector<Bits>& framesIn, const InferOptions& opt = {}) {
    std::vector<InferredField> out;
    std::vector<Bits> frames;
    for (const auto& f : framesIn)
        if (!f.empty() && f.size() <= opt.maxBits && frames.size() < opt.maxFrames) frames.push_back(f);
    if (frames.size() < 2) return out;
    size_t minLen = frames[0].size(), maxLen = minLen;
    for (const auto& f : frames) { minLen = std::min(minLen, f.size()); maxLen = std::max(maxLen, f.size()); }
    const auto cols = diffFrames(frames);

    // 1. constant runs and varying BYTES over [0, minLen). A byte with any varying column is one varying unit
    // (its constant bits belong to it: a flag byte with two values stays one field); constant bytes merge.
    struct Run { size_t pos, len; bool constant; };
    std::vector<Run> runs;
    std::vector<bool> byteVarying((minLen + 7) / 8, false);
    for (size_t i = 0; i < minLen; ++i)
        if (!cols[i].constant()) byteVarying[i / 8] = true;
    for (size_t b = 0; b < byteVarying.size(); ++b) {
        const size_t pos = b * 8, len = std::min<size_t>(8, minLen - pos);
        if (!byteVarying[b] && !runs.empty() && runs.back().constant) runs.back().len += len;
        else runs.push_back({pos, len, !byteVarying[b]});
    }
    // merge adjacent varying runs back into one candidate stretch for counter/length detection
    auto valueOf = [&](const Bits& f, size_t pos, size_t len) { return getField(f, pos, len, true); };
    std::vector<InferredField> fields;
    for (const auto& r : runs) {
        InferredField f;
        f.pos = r.pos;
        f.len = r.len;
        f.evidence = frames.size();
        if (r.constant) {
            f.kind = FieldKind::Constant;
            f.value = r.len <= 64 ? valueOf(frames[0], r.pos, r.len) : 0;
            f.distinct = 1;
            f.label = "const_" + std::to_string(r.pos);
        } else {
            f.kind = FieldKind::Data;
            f.label = "data_" + std::to_string(r.pos);
        }
        fields.push_back(std::move(f));
    }
    // 2. counters: try byte-aligned 8/16/24/32-bit windows over varying fields, longest first; a counter claims the bits
    std::vector<bool> taken(minLen, false);
    for (unsigned width : {32u, 24u, 16u, 8u}) {
        for (size_t pos = 0; pos + width <= minLen; pos += 8) {
            // every byte of the window must vary (a constant byte is not part of a counter) and be unclaimed
            bool varyingAll = true;
            for (size_t k = 0; k < width && varyingAll; k += 8) varyingAll &= byteVarying[(pos + k) / 8];
            for (size_t k = 0; k < width && varyingAll; ++k) varyingAll &= !taken[pos + k];
            if (!varyingAll) continue;
            const uint64_t mod = 1ull << width;
            std::vector<uint64_t> v;
            for (const auto& f : frames) v.push_back(valueOf(f, pos, width));
            std::vector<uint64_t> steps;
            for (size_t i = 1; i < v.size(); ++i) steps.push_back((v[i] - v[i - 1]) & (mod - 1));
            // most common step
            uint64_t bestStep = 0;
            size_t bestCount = 0;
            for (uint64_t s : steps) {
                const size_t c = static_cast<size_t>(std::count(steps.begin(), steps.end(), s));
                if (c > bestCount) { bestCount = c; bestStep = s; }
            }
            if (bestStep == 0 || steps.size() < 2) continue;
            if (static_cast<double>(bestCount) / static_cast<double>(steps.size()) < opt.counterShare) continue;
            // a field that merely has few values (a 2-valued flag stepping 0,1,0,1) is not a counter
            std::vector<uint64_t> d(v);
            std::sort(d.begin(), d.end());
            d.erase(std::unique(d.begin(), d.end()), d.end());
            if (d.size() < 3) continue;
            for (size_t k = 0; k < width; ++k) taken[pos + k] = true;
            InferredField f;
            f.pos = pos;
            f.len = width;
            f.kind = FieldKind::Counter;
            f.value = bestStep;
            f.distinct = static_cast<unsigned>(d.size());
            f.evidence = steps.size();
            f.label = "counter_" + std::to_string(pos);
            f.note = bestCount == steps.size() ? "every consecutive pair steps by " + std::to_string(bestStep)
                                               : std::to_string(bestCount) + " of " + std::to_string(steps.size()) + " consecutive pairs step by " + std::to_string(bestStep) + " (frames missed?)";
            fields.push_back(std::move(f));
        }
    }
    // 3. a length field (only when the frame lengths differ)
    if (maxLen != minLen) {
        for (unsigned width : {16u, 8u})
            for (size_t pos = 0; pos + width <= minLen; pos += 8) {
                bool free = true;
                for (size_t k = 0; k < width; ++k) free &= !taken[pos + k];
                if (!free) continue;
                for (bool inBytes : {true, false}) {
                    // value == length(frame in unit) + c, c constant
                    long long c0 = 0;
                    bool all = true, first = true;
                    for (const auto& f : frames) {
                        const long long unit = inBytes ? static_cast<long long>(f.size() / 8) : static_cast<long long>(f.size());
                        const long long c = static_cast<long long>(valueOf(f, pos, width)) - unit;
                        if (first) { c0 = c; first = false; }
                        else if (c != c0) { all = false; break; }
                    }
                    if (!all || c0 < -64 || c0 > 64) continue;
                    for (size_t k = 0; k < width; ++k) taken[pos + k] = true;
                    InferredField f;
                    f.pos = pos;
                    f.len = width;
                    f.kind = FieldKind::Length;
                    f.value = static_cast<uint64_t>(c0);
                    f.lengthInBytes = inBytes;
                    f.evidence = frames.size();
                    f.label = "length_" + std::to_string(pos);
                    f.note = std::string("equals the frame length in ") + (inBytes ? "bytes" : "bits") + (c0 ? " plus " + std::to_string(c0) : "");
                    fields.push_back(std::move(f));
                    goto lengthDone;
                }
            }
    lengthDone:;
    }
    // 4. checksum position from crc.hpp
    if (frames.size() >= 3 && detail::wholeBytes(frames)) {
        std::vector<std::vector<uint8_t>> bytes;
        for (const auto& f : frames) bytes.push_back(toBytes(f));
        auto place = [&](size_t cposBytes, unsigned nbytes, const std::string& name, const std::string& note) {
            InferredField f;
            f.pos = cposBytes * 8;
            f.len = nbytes * 8;
            f.kind = FieldKind::Checksum;
            f.evidence = frames.size();
            f.label = "checksum";
            f.note = name + (note.empty() ? "" : "; " + note);
            for (size_t k = f.pos; k < f.pos + f.len && k < minLen; ++k) taken[k] = true;
            fields.push_back(std::move(f));
        };
        // frames must be the same length for the byte positions to mean the same thing
        bool sameLen = maxLen == minLen;
        if (sameLen) {
            const auto crcs = findCrc(bytes);
            const auto sums = findChecksums(bytes);
            if (!crcs.empty() && crcs[0].catalogueMatch && crcs[0].coincidence < 1e-3) {
                const auto& h = crcs[0];
                place(bytes[0].size() - h.tailBytes - h.model.width / 8, h.model.width / 8, h.model.name, h.note);
            } else if (!crcs.empty() && crcs[0].derived && crcs[0].coincidence < 1e-3) {
                const auto& h = crcs[0];
                place(bytes[0].size() - h.tailBytes - h.model.width / 8, h.model.width / 8,
                      "CRC-" + std::to_string(h.model.width) + " poly 0x" + [&] { char b[20]; std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(h.model.poly)); return std::string(b); }(), h.note);
            } else if (!sums.empty() && frames.size() >= 4) {
                const auto& h = sums[0];
                place(bytes[0].size() - h.tailBytes - sumBytes(h.kind), sumBytes(h.kind), sumName(h.kind), sums.size() > 1 ? std::to_string(sums.size()) + " checksum readings fit" : "");
            }
        }
    }
    // 5. classify the leftover varying fields: flags or data; and drop covered ones
    std::vector<InferredField> result;
    for (auto& f : fields) {
        if (f.kind == FieldKind::Data || f.kind == FieldKind::Constant) {
            // skip any part already claimed (a counter, length or checksum window)
            bool covered = false;
            for (size_t k = f.pos; k < f.pos + f.len; ++k) covered |= taken[k];
            if (covered) {
                // keep the uncovered stretches as separate Data fields
                size_t i = f.pos;
                while (i < f.pos + f.len) {
                    while (i < f.pos + f.len && taken[i]) ++i;
                    size_t j = i;
                    while (j < f.pos + f.len && !taken[j]) ++j;
                    if (j > i) {
                        InferredField g;
                        g.pos = i;
                        g.len = j - i;
                        g.kind = f.kind;
                        g.evidence = frames.size();
                        if (f.kind == FieldKind::Constant) {
                            g.value = g.len <= 64 ? valueOf(frames[0], i, g.len) : 0;
                            g.distinct = 1;
                            g.label = "const_" + std::to_string(i);
                        } else {
                            g.label = "data_" + std::to_string(i);
                        }
                        result.push_back(std::move(g));
                    }
                    i = j;
                }
                continue;
            }
        }
        result.push_back(std::move(f));
    }
    for (auto& f : result) {
        if (f.kind == FieldKind::Data && f.len <= 32) {
            std::vector<uint64_t> v;
            for (const auto& fr : frames) v.push_back(valueOf(fr, f.pos, f.len));
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            f.distinct = static_cast<unsigned>(v.size());
            if (v.size() <= opt.flagsMaxDistinct && frames.size() >= 8) {
                f.kind = FieldKind::Flags;
                f.label = "flags_" + std::to_string(f.pos);
                f.note = std::to_string(v.size()) + " distinct values in " + std::to_string(frames.size()) + " frames";
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const InferredField& a, const InferredField& b) { return a.pos < b.pos; });
    out = std::move(result);
    return out;
}

}  // namespace cascade::bits
