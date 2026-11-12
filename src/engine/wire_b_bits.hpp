// wire_b_bits.hpp - the BIT VIEW library (core/bits/*.hpp) as engine operations: keep a burst of a radio as level runs, decode it again with chosen settings, search the line codes,
// find a CRC or a checksum, infer fields, diff two frames, check and run a user-described decoder, check a code table (wire-b; docs/multi-radio-records/wire-b.md).
//
// PURE: no engine, no thread, no file. The engine glue (engine_wire_b.cpp) owns one BitsRadio per radio, hands in the newest span of the radio channel I/Q for KEEP, and keeps each
// answer in a LOCAL fact "radio.<n>.bits.<name>". THE SLICER TAP: no per-sample hook is added to the receive path; KEEP slices the span on demand (envelope -> two levels -> level runs in
// microseconds, core/bits/ookslicer.hpp) into the radio bounded ring (256 slices, about a million runs).
//
// PARAMETERS are one JSON object (every key optional, an unknown key is an error naming it); decoder-file text and table text are the op text itself. Frames and bits never leave the
// engine except in the LOCAL fact, and nothing is written to disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/bits/bitedit.hpp"
#include "core/bits/bits.hpp"
#include "core/bits/bitspec.hpp"
#include "core/bits/crc.hpp"
#include "core/bits/fields.hpp"
#include "core/bits/msgdiff.hpp"
#include "core/bits/rawbuf.hpp"
#include "engine/wire_b_sigmeas.hpp"

namespace cascade::engine::wireb {

namespace cb = cascade::bits;

// What one radio keeps for the bit view.
struct BitsRadio {
    cb::RawRing ring;
    std::vector<cb::Frame> frames;   // the frames of the last REDECODE, numbered from 1
};

enum class BitsOp { Keep, List, Redecode, Sweep, FindCrc, FindSum, Infer, Diff, SpecCheck, SpecRun, TableCheck };

namespace bitsdetail {

struct CodeName { const char* word; cb::LineCode code; };
inline const CodeName* codeNames(std::size_t& n) {
    static const CodeName t[] = {
        {"nrz-l", cb::LineCode::NrzL},
        {"nrz-m", cb::LineCode::NrzM},
        {"nrz-s", cb::LineCode::NrzS},
        {"manchester", cb::LineCode::ManchesterIeee},
        {"manchester-thomas", cb::LineCode::ManchesterThomas},
        {"diff-manchester", cb::LineCode::DiffManchester},
        {"diff-manchester-inv", cb::LineCode::DiffManchesterInv},
        {"biphase-mark", cb::LineCode::BiphaseMark},
        {"biphase-space", cb::LineCode::BiphaseSpace},
        {"miller", cb::LineCode::Miller},
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}
inline bool codeFromWord(const std::string& w, cb::LineCode& out) {
    std::size_t n = 0;
    const CodeName* t = codeNames(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (w == t[i].word) { out = t[i].code; return true; }
    }
    return false;
}
inline const char* wordOfCode(cb::LineCode c) {
    std::size_t n = 0;
    const CodeName* t = codeNames(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i].code == c) { return t[i].word; }
    }
    return "?";
}

inline std::string hexOf(const std::vector<std::uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (const std::uint8_t x : b) { s.push_back(d[x >> 4]); s.push_back(d[x & 15u]); }
    return s;
}

inline std::string bitText(const cb::Bits& b, std::size_t cap = 4096) {
    std::string s;
    const std::size_t n = std::min(b.size(), cap);
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { s.push_back(b[i] != 0 ? '1' : '0'); }
    return s;
}

// Reads the chain settings out of the parameters. The keys: line_codes (a list of words, one to three stages), invert, balance, chip_us, phase (-1 auto, 0, 1), sync (a bit string),
// sync_errors, both_polarities, framing ("none", "fixed", "flag"), fixed_bits, flag (bit string), unstuff, whiten ("none", "pn9"), max_frames, max_frame_bits.
inline bool readChain(detail::Params& q, const Json& p, cb::ChainSettings& cs) {
    if (p.contains("line_codes")) {
        const Json& a = p["line_codes"];
        if (!a.is_array() || a.empty() || a.size() > 3) { q.error = "\"line_codes\" is a list of one to three line-code words"; return false; }
        cs.stages.clear();
        for (const Json& w : a) {
            cb::LineCode c;
            if (!w.is_string() || !codeFromWord(w.get<std::string>(), c)) { q.error = "\"line_codes\" holds a word that is not a line code (nrz-l, nrz-m, nrz-s, manchester, manchester-thomas, diff-manchester, diff-manchester-inv, biphase-mark, biphase-space, miller)"; return false; }
            cs.stages.push_back(c);
        }
    }
    q.seen.insert("line_codes");
    double phase = cs.phase, chip = cs.chipUs, errs = static_cast<double>(cs.syncErrors), fixedBits = static_cast<double>(cs.fixedBits);
    std::size_t maxFrames = cs.maxFrames, maxFrameBits = cs.maxFrameBits;
    std::string sync, flag, framing = "none", whiten = "none";
    if (!q.flag("invert", cs.invert) || !q.flag("balance", cs.balance) || !q.number("chip_us", chip, 0.0, 1e6) || !q.number("phase", phase, -1.0, 1.0) ||
        !q.word("sync", sync) || !q.number("sync_errors", errs, 0.0, 16.0) || !q.flag("both_polarities", cs.syncBothPolarities) || !q.word("framing", framing) ||
        !q.number("fixed_bits", fixedBits, 0.0, 8192.0) || !q.word("flag", flag) || !q.flag("unstuff", cs.unstuff) || !q.word("whiten", whiten) ||
        !q.count("max_frames", maxFrames, 1, 4096) || !q.count("max_frame_bits", maxFrameBits, 8, 65536)) { return false; }
    cs.chipUs = chip; cs.phase = static_cast<int>(phase); cs.syncErrors = static_cast<std::size_t>(errs); cs.fixedBits = static_cast<std::size_t>(fixedBits);
    cs.maxFrames = maxFrames; cs.maxFrameBits = maxFrameBits;
    if (!sync.empty()) {
        bool ok = true;
        cs.sync = cb::fromString(sync, &ok);
        if (!ok || cs.sync.size() > 64) { q.error = "\"sync\" is a string of 0 and 1, at most 64 bits"; return false; }
    }
    if (!flag.empty()) {
        bool ok = true;
        cs.flag = cb::fromString(flag, &ok);
        if (!ok || cs.flag.empty() || cs.flag.size() > 32) { q.error = "\"flag\" is a string of 0 and 1, one to 32 bits"; return false; }
    }
    if (framing == "none") { cs.framing = cb::FramingKind::None; }
    else if (framing == "fixed") { cs.framing = cb::FramingKind::Fixed; if (cs.fixedBits == 0) { q.error = "\"fixed_bits\" is needed for fixed framing"; return false; } }
    else if (framing == "flag") { cs.framing = cb::FramingKind::Flag; }
    else { q.error = "\"framing\" is none, fixed or flag"; return false; }
    if (whiten == "pn9") { cs.whiten.kind = cb::WhitenKind::AdditiveBits; cs.whiten.lfsr = cb::kPn9; cs.whiten.seed = cb::kPn9Seed; }
    else if (whiten != "none") { q.error = "\"whiten\" is none or pn9"; return false; }
    return true;
}

}  // namespace bitsdetail

inline RunResult bitsRun(BitsOp op, BitsRadio& st, const sm::Cf* iq, std::size_t nIq, double iqRate, const std::string& text, std::int64_t ival0, const Json& p) {
    using bitsdetail::bitText;
    using bitsdetail::hexOf;
    RunResult rr;
    detail::Params q(p);
    Json out = Json::object();
    const auto refuse = [&rr, &q]() { rr.ok = false; rr.why = q.error; return rr; };
    const auto sliceOf = [&](double id, cb::RawSlice& s) -> bool {
        if (id <= 0.0) {
            const auto all = st.ring.snapshot();
            if (all.empty()) { return false; }
            s = all.back();
            return true;
        }
        return st.ring.get(static_cast<std::uint64_t>(id), &s);
    };
    switch (op) {
        case BitsOp::Keep: {
            double endAgo = 0.0, dur = 0.0;
            q.number("end_ago_s", endAgo, 0.0, 60.0); q.number("duration_s", dur, 0.001, 10.0);
            cb::SliceParams sp;
            sp.sampleRate = iqRate;
            std::string label;
            if (!q.word("label", label) || !q.number("smooth_us", sp.smoothUs, 1.0, 10000.0) || !q.number("threshold", sp.thresholdFraction, 0.05, 0.95) ||
                !q.number("min_span_ratio", sp.minSpanRatio, 1.0, 1000.0) || !q.finish()) { return refuse(); }
            std::vector<float> flat(nIq * 2);
            for (std::size_t i = 0; i < nIq; ++i) { flat[2 * i] = iq[i].real(); flat[2 * i + 1] = iq[i].imag(); }
            const cb::SliceResult s = cb::sliceEnvelope(cb::iqMagnitude(flat), sp);
            rr.ok = true;
            rr.valid = s.signal && !s.runs.empty();
            out["signal"] = s.signal; out["runs"] = s.runs.size(); out["floor"] = s.floorLevel; out["peak"] = s.peakLevel; out["threshold"] = s.threshold;
            if (!rr.valid) {
                rr.reason = "no signal: the peak is not over the floor by the minimum ratio";
            } else {
                cb::RawSlice slice;
                slice.label = label;
                slice.runs = s.runs;
                const std::uint64_t id = st.ring.push(std::move(slice));
                if (id == 0) { rr.valid = false; rr.reason = "the burst alone is over the ring limit"; }
                out["id"] = id;
            }
            out["kept"] = st.ring.size();
            rr.out = std::move(out);
            return rr;
        }
        case BitsOp::List: {
            if (!q.finish()) { return refuse(); }
            Json list = Json::array();
            for (const cb::RawSlice& s : st.ring.snapshot()) {
                double total = 0.0;
                for (const cb::LevelRun& r : s.runs) { total += r.duration; }
                list.push_back({{"id", s.id}, {"label", s.label}, {"runs", s.runs.size()}, {"length_us", total}});
            }
            out["slices"] = list; out["evicted"] = st.ring.evicted(); out["refused"] = st.ring.refused();
            rr.ok = true; rr.valid = true; rr.out = std::move(out);
            return rr;
        }
        case BitsOp::Redecode:
        case BitsOp::Sweep: {
            double id = 0.0;
            cb::ChainSettings cs;
            if (!q.number("slice", id, 0.0, 1e15) || !bitsdetail::readChain(q, p, cs) || !q.finish()) { return refuse(); }
            cb::RawSlice slice;
            if (!sliceOf(id, slice)) { q.error = "no such slice (KEEP a burst first)"; return refuse(); }
            rr.ok = true;
            if (op == BitsOp::Sweep) {
                std::vector<cb::LineCode> all;
                for (std::size_t i = 0; i < cb::kLineCodeCount; ++i) { all.push_back(static_cast<cb::LineCode>(i)); }
                const std::vector<cb::SweepRow> rows = cb::sweepSettings(slice, cs, all);
                Json list = Json::array();
                for (const cb::SweepRow& r : rows) {
                    list.push_back({{"line_code", bitsdetail::wordOfCode(r.code)}, {"invert", r.invert}, {"sync_hits", r.syncHits}, {"violations", r.violations}, {"bits", r.bits}});
                }
                out["slice"] = slice.id; out["rows"] = list;
                rr.valid = !rows.empty();
                if (!rr.valid) { rr.reason = "no setting could read this slice"; }
                rr.out = std::move(out);
                return rr;
            }
            const cb::ChainResult r = cb::redecode(slice, cs);
            rr.valid = r.ok;
            rr.reason = r.ok ? "" : r.problem;
            st.frames = r.frames;
            out["slice"] = slice.id; out["chip_us"] = r.chipUs; out["violations"] = r.violations; out["bit_count"] = r.bits.size(); out["bits"] = bitText(r.bits);
            out["sync_hits"] = r.syncHits.size();
            Json frames = Json::array();
            std::size_t i = 0;
            for (const cb::Frame& f : r.frames) {
                ++i;
                frames.push_back({{"n", i}, {"start", f.start}, {"bit_count", f.bits.size()}, {"hex", hexOf(cb::toBytes(f.bits))}, {"bits", bitText(f.bits, 1024)}, {"sync_distance", f.syncDistance}});
            }
            out["frames"] = frames;
            rr.out = std::move(out);
            return rr;
        }
        case BitsOp::FindCrc:
        case BitsOp::FindSum: {
            std::size_t maxHead = 4, maxTail = 2, maxHits = 32;
            std::vector<unsigned> widths = {8, 16, 24, 32};
            if (!q.count("max_head_bytes", maxHead, 0, 16) || !q.count("max_tail_bytes", maxTail, 0, 16) || !q.count("max_hits", maxHits, 1, 256)) { return refuse(); }
            if (p.contains("widths")) {
                const Json& w = p["widths"];
                if (!w.is_array() || w.empty() || w.size() > 8) { q.error = "\"widths\" is a list of one to eight CRC widths"; return refuse(); }
                widths.clear();
                for (const Json& e : w) {
                    if (!e.is_number_integer() || e.get<int>() < 1 || e.get<int>() > 64) { q.error = "\"widths\" holds a width outside 1 to 64"; return refuse(); }
                    widths.push_back(static_cast<unsigned>(e.get<int>()));
                }
            }
            q.seen.insert("widths");
            if (!q.finish()) { return refuse(); }
            std::vector<std::vector<std::uint8_t>> frames;
            for (const cb::Frame& f : st.frames) {
                std::vector<std::uint8_t> b = cb::toBytes(f.bits);
                if (!b.empty()) { frames.push_back(std::move(b)); }
            }
            if (frames.size() < 3) { q.error = "decode at least three frames first (REDECODE)"; return refuse(); }
            rr.ok = true;
            out["frames"] = frames.size();
            Json hits = Json::array();
            if (op == BitsOp::FindCrc) {
                cb::CrcFindOptions o;
                o.widths = widths; o.maxHeadBytes = maxHead; o.maxTailBytes = maxTail; o.maxHits = maxHits;
                for (const cb::CrcHit& h : cb::findCrc(frames, o)) {
                    hits.push_back({{"width", h.model.width}, {"poly", h.model.poly}, {"init", h.model.init}, {"xorout", h.model.xorout}, {"refin", h.model.refin}, {"refout", h.model.refout},
                                    {"name", h.model.name}, {"catalogue", h.catalogueMatch}, {"byte_order", h.order == cb::ByteOrder::Big ? "big" : "little"}, {"head_bytes", h.headBytes},
                                    {"tail_bytes", h.tailBytes}, {"derived", h.derived}, {"candidates", h.candidates}, {"free_dims", h.freeDims}, {"coincidence", h.coincidence}, {"note", h.note}});
                }
            } else {
                for (const cb::SumHit& h : cb::findChecksums(frames, maxHead, maxTail)) {
                    hits.push_back({{"kind", static_cast<int>(h.kind)}, {"byte_order", h.order == cb::ByteOrder::Big ? "big" : "little"}, {"head_bytes", h.headBytes}, {"tail_bytes", h.tailBytes}});
                }
            }
            out["hits"] = hits;
            rr.valid = !hits.empty();
            if (!rr.valid) { rr.reason = op == BitsOp::FindCrc ? "no CRC fits every frame" : "no checksum fits every frame"; }
            rr.out = std::move(out);
            return rr;
        }
        case BitsOp::Infer: {
            if (!q.finish()) { return refuse(); }
            std::vector<cb::Bits> frames;
            for (const cb::Frame& f : st.frames) { frames.push_back(f.bits); }
            if (frames.size() < 2) { q.error = "decode at least two frames first (REDECODE)"; return refuse(); }
            rr.ok = true;
            Json list = Json::array();
            for (const cb::InferredField& f : cb::inferFields(frames)) {
                list.push_back({{"pos", f.pos}, {"len", f.len}, {"kind", cb::fieldKindName(f.kind)}, {"value", f.value}, {"distinct", f.distinct}, {"evidence", f.evidence},
                                {"length_in_bytes", f.lengthInBytes}, {"label", f.label}, {"note", f.note}});
            }
            out["frames"] = frames.size(); out["fields"] = list;
            rr.valid = !list.empty();
            if (!rr.valid) { rr.reason = "no field could be told apart"; }
            rr.out = std::move(out);
            return rr;
        }
        case BitsOp::Diff: {
            std::size_t a = 0, b = 0;
            if (!q.count("a", a, 1, 4096) || !q.count("b", b, 1, 4096) || !q.finish()) { return refuse(); }
            if (a == 0 || b == 0 || a > st.frames.size() || b > st.frames.size()) { q.error = "frame numbers are from 1 to the number of decoded frames"; return refuse(); }
            const cb::BitDiff d = cb::diffBits(st.frames[a - 1].bits, st.frames[b - 1].bits);
            rr.ok = true;
            rr.valid = d.ok;
            rr.reason = d.ok ? "" : "a frame is over the diff limit";
            out["a"] = a; out["b"] = b; out["row_a"] = d.rowA; out["row_b"] = d.rowB; out["marks"] = d.marks; out["matches"] = d.matches; out["mismatches"] = d.mismatches; out["gaps"] = d.gaps;
            rr.out = std::move(out);
            return rr;
        }
        case BitsOp::SpecCheck:
        case BitsOp::SpecRun: {
            if (text.size() > (1u << 16)) { q.error = "the decoder text is longer than 65536 bytes"; return refuse(); }
            const cb::SpecParse sp = cb::parseSpec(text);
            Json errs = Json::array();
            for (const cb::SpecError& e : sp.errors) { errs.push_back({{"line", e.line}, {"message", e.message}}); }
            rr.ok = true;
            out["ok"] = sp.ok; out["errors"] = errs;
            if (!sp.ok) { rr.valid = false; rr.reason = "the decoder text has errors (see the list)"; rr.out = std::move(out); return rr; }
            out["name"] = sp.spec.name;
            if (op == BitsOp::SpecCheck) { rr.valid = true; rr.out = std::move(out); return rr; }
            Json msgs = Json::array();
            std::size_t scanned = 0;
            for (const cb::RawSlice& s : st.ring.snapshot()) {
                if (ival0 > 0 && static_cast<std::int64_t>(s.id) != ival0) { continue; }
                ++scanned;
                cb::SpecStats stats;
                for (const cb::SpecMessage& m : cb::runSpec(sp.spec, s.runs, &stats, 64)) {
                    Json vals = Json::array();
                    for (const cb::SpecValue& v : m.values) { vals.push_back({{"name", v.name}, {"text", v.text}, {"value", v.value}, {"valid", v.valid}}); }
                    msgs.push_back({{"slice", s.id}, {"start_us", m.startUs}, {"bits", bitText(m.frame, 1024)}, {"inverted", m.inverted}, {"violations", m.violations}, {"values", vals},
                                    {"check", m.check == cb::SpecCheckResult::Ok ? "ok" : m.check == cb::SpecCheckResult::Bad ? "bad" : "none"}, {"plausible", m.plausible}});
                    if (msgs.size() >= 256) { break; }
                }
            }
            out["slices_scanned"] = scanned; out["messages"] = msgs;
            rr.valid = !msgs.empty();
            if (!rr.valid) { rr.reason = scanned == 0 ? "no kept slice to run it on" : "no message matched"; }
            rr.out = std::move(out);
            return rr;
        }
        case BitsOp::TableCheck: {
            if (text.size() > (1u << 16)) { q.error = "the table text is longer than 65536 bytes"; return refuse(); }
            const cb::TableParse t = cb::parseTable(text);
            Json errs = Json::array();
            for (const cb::TableError& e : t.errors) { errs.push_back({{"line", e.line}, {"message", e.message}}); }
            out["ok"] = t.ok; out["errors"] = errs;
            rr.ok = true; rr.valid = t.ok; rr.reason = t.ok ? "" : "the table text has errors (see the list)";
            rr.out = std::move(out);
            return rr;
        }
    }
    rr.ok = false;
    rr.why = "no such bit-view operation";
    return rr;
}

}  // namespace cascade::engine::wireb
