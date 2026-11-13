// wire_b_cursors.hpp - the eight-cursor tool, the harmonic / sideband cursors and the frame log (dsp/sigmeas_cursors.hpp, dsp/sigmeas_framelog.hpp) as engine operations (wire-b).
//
// PURE like wire_b_sigmeas.hpp: JSON parameters in, a JSON answer out; the engine glue owns one CursorSet8 per radio and one FrameLog and takes the span from the radio's channel I/Q.
// Screen lane K draws its own eight-cursor tool in the host; these operations are for the web page, plugins and persistence of a cursor set held by the engine.
//
// THE FRAME LOG keeps decoded frames IN MEMORY ONLY, with the CRC flag a decoder gave them, so that a frame that FAILED its check can be shown on request (a person is the judge of whether the
// bytes are worth anything). No plugin call fills it yet (that is an ABI request); frames never go to disk and never into an upload or a log; they are in a LOCAL fact only when asked for.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/sigmeas_cursors.hpp"
#include "dsp/sigmeas_framelog.hpp"
#include "engine/wire_b_sigmeas.hpp"

namespace cascade::engine::wireb {

enum class CursorOp { Set, Read, Marks };

namespace cursordetail {

inline Json readoutJson(const sm::CursorSet8& cs) {
    Json list = Json::array();
    for (const sm::CursorReadout& r : cs.readout()) {
        list.push_back({{"cursor", r.index + 1}, {"freq_hz", r.freqHz}, {"offset_hz", detail::numberOrNull(r.offsetHz)}, {"ratio", detail::numberOrNull(r.ratio)},
                        {"level_dbfs", detail::numberOrNull(r.levelDbfs)}, {"in_range", r.inRange}});
    }
    return list;
}

inline const char* modeWord(sm::CursorMode m) { return m == sm::CursorMode::Free ? "free" : m == sm::CursorMode::LockedSpacing ? "locked" : "harmonic"; }

}  // namespace cursordetail

// SET changes the radio's cursor set (a copy is changed and kept only if every part was accepted); READ and MARKS measure on the span.
inline RunResult cursorRun(CursorOp op, sm::CursorSet8& set, const sm::Cf* iq, std::size_t nIq, double iqRate, const Json& p) {
    using detail::numberOrNull;
    RunResult rr;
    detail::Params q(p);
    double endAgo = 0.0, dur = 0.0;
    if (op != CursorOp::Set) { q.number("end_ago_s", endAgo, 0.0, 60.0); q.number("duration_s", dur, 0.001, 10.0); }
    Json out = Json::object();
    const auto refuse = [&rr, &q]() { rr.ok = false; rr.why = q.error; return rr; };
    if (op == CursorOp::Set) {
        sm::CursorSet8 c = set;
        std::string mode;
        double spacing = 0.0, ref = 0.0, lo = 0.0, hi = 0.0;
        if (!q.word("mode", mode) || !q.number("spacing_hz", spacing, -1e12, 1e12) || !q.number("reference", ref, 1.0, 8.0) || !q.number("range_low_hz", lo, -1e12, 1e12) ||
            !q.number("range_high_hz", hi, -1e12, 1e12)) { return refuse(); }
        q.seen.insert("set");
        if (p.contains("range_low_hz") != p.contains("range_high_hz")) { q.error = "range_low_hz and range_high_hz go together"; return refuse(); }
        if (p.contains("range_low_hz") && !c.setRange(lo, hi)) { q.error = "the range must have its high end above its low end"; return refuse(); }
        if (p.contains("mode")) {
            if (mode == "free") { c.setMode(sm::CursorMode::Free); }
            else if (mode == "locked") { c.setMode(sm::CursorMode::LockedSpacing); }
            else if (mode == "harmonic") { if (!c.setMode(sm::CursorMode::Harmonic)) { q.error = "harmonic mode needs the first cursor off zero"; return refuse(); } }
            else { q.error = "\"mode\" is free, locked or harmonic"; return refuse(); }
        }
        if (p.contains("spacing_hz") && !c.setSpacing(spacing)) { q.error = "the spacing cannot be set in harmonic mode"; return refuse(); }
        if (p.contains("reference") && !c.setReference(static_cast<std::size_t>(ref) - 1)) { q.error = "\"reference\" is 1 to 8"; return refuse(); }
        if (p.contains("set")) {
            const Json& a = p["set"];
            if (!a.is_array() || a.size() > 8) { q.error = "\"set\" is a list of at most eight {\"cursor\": 1-8, \"freq_hz\": number}"; return refuse(); }
            for (const Json& e : a) {
                if (!e.is_object() || !e.contains("cursor") || !e.contains("freq_hz") || !e["cursor"].is_number_integer() || !e["freq_hz"].is_number()) { q.error = "\"set\" holds an entry that is not {\"cursor\": 1-8, \"freq_hz\": number}"; return refuse(); }
                const int i = e["cursor"].get<int>();
                const double f = e["freq_hz"].get<double>();
                if (i < 1 || i > 8 || !std::isfinite(f) || std::fabs(f) > 1e12) { q.error = "\"set\" holds a cursor outside 1 to 8 or a frequency outside 1e12 Hz"; return refuse(); }
                if (!c.setCursor(static_cast<std::size_t>(i - 1), f)) { q.error = "that cursor cannot be put there in this mode (harmonic mode takes no zero)"; return refuse(); }
            }
        }
        if (!q.finish()) { return refuse(); }
        set = c;
        out["mode"] = cursordetail::modeWord(c.mode()); out["spacing_hz"] = c.spacingHz(); out["reference"] = c.reference() + 1; out["cursors"] = cursordetail::readoutJson(c);
        out["mean_spacing_hz"] = c.meanSpacingHz(); out["spacing_rms_hz"] = c.spacingRmsHz();
        rr.ok = true; rr.valid = true; rr.out = std::move(out);
        return rr;
    }
    const sm::Psd psd = sm::welchPsd(iq, nIq, iqRate);
    if (op == CursorOp::Read) {
        double search = 0.0;
        bool snap = false;
        if (!q.number("search_hz", search, 0.0, 1e12) || !q.flag("snap", snap) || !q.finish()) { return refuse(); }
        sm::CursorSet8 c = set;
        const double s = search > 0.0 ? search : 3.0 * psd.binHz;
        bool ok = psd.valid && c.layLevels(psd, s);
        bool snapped = false;
        if (ok && snap) { snapped = c.snapToPeaks(psd, s); }
        rr.ok = true;
        rr.valid = ok;
        rr.reason = ok ? "" : (psd.valid ? "a cursor lies outside the span" : (psd.reason == nullptr ? "span too short" : psd.reason));
        if (ok) { set = c; }
        out["bin_hz"] = psd.binHz; out["snapped"] = snapped; out["mode"] = cursordetail::modeWord(c.mode()); out["cursors"] = cursordetail::readoutJson(c);
        rr.out = std::move(out);
        return rr;
    }
    // Marks: harmonic or sideband cursors with their levels.
    std::string kind = "harmonic";
    double base = 0.0, spacing = 0.0, count = 5.0, lo = -iqRate / 2.0, hi = iqRate / 2.0, search = 0.0;
    if (!q.word("kind", kind) || !q.number("base_hz", base, -1e12, 1e12) || !q.number("carrier_hz", base, -1e12, 1e12) || !q.number("spacing_hz", spacing, 0.0, 1e12) ||
        !q.number("count", count, 1.0, 1000.0) || !q.number("low_hz", lo, -1e12, 1e12) || !q.number("high_hz", hi, -1e12, 1e12) || !q.number("search_hz", search, 0.0, 1e12) || !q.finish()) { return refuse(); }
    std::vector<sm::CursorMark> marks;
    if (kind == "harmonic") { marks = sm::harmonicCursors(base, lo, hi, static_cast<std::size_t>(count)); }
    else if (kind == "sideband") { marks = sm::sidebandCursors(base, spacing, static_cast<int>(count), lo, hi); }
    else { q.error = "\"kind\" is harmonic or sideband"; return refuse(); }
    if (psd.valid) { sm::readLevels(psd, marks, search > 0.0 ? search : 3.0 * psd.binHz); }
    Json list = Json::array();
    for (const sm::CursorMark& m : marks) {
        const char* k = m.kind == sm::MarkKind::Fundamental ? "fundamental" : m.kind == sm::MarkKind::Harmonic ? "harmonic" : m.kind == sm::MarkKind::UpperSideband ? "upper" :
                        m.kind == sm::MarkKind::LowerSideband ? "lower" : "carrier";
        list.push_back({{"freq_hz", m.freqHz}, {"order", m.order}, {"kind", k}, {"snapped_hz", numberOrNull(m.snappedHz)}, {"level_dbfs", numberOrNull(m.levelDbfs)}, {"in_range", m.inRange}});
    }
    out["marks"] = list; out["bin_hz"] = psd.binHz;
    rr.ok = true; rr.valid = psd.valid && !marks.empty();
    rr.reason = rr.valid ? "" : (marks.empty() ? "no mark falls inside the range" : "span too short");
    rr.out = std::move(out);
    return rr;
}

inline Json frameLogJson(const sm::FrameLog& log, bool includeBad) {
    static const char* hexd = "0123456789abcdef";
    Json frames = Json::array();
    for (const sm::FrameRecord& r : log.snapshot(includeBad)) {
        std::string hex;
        for (const std::uint8_t b : r.bytes) { hex.push_back(hexd[b >> 4]); hex.push_back(hexd[b & 15u]); }
        frames.push_back({{"seq", r.seq}, {"time_s", r.timeS}, {"freq_hz", r.freqHz}, {"length", r.bytes.size()}, {"hex", hex}, {"has_crc", r.hasCrc}, {"crc_ok", r.crcOk},
                          {"crc_received", r.crcReceived}, {"crc_computed", r.crcComputed}, {"confidence", detail::numberOrNull(r.confidence)}});
    }
    const sm::FrameLog::Counts c = log.counts();
    Json out = Json::object();
    out["keep_bad"] = log.keepBad(); out["include_bad"] = includeBad; out["good_total"] = c.goodTotal; out["bad_total"] = c.badTotal; out["held"] = c.held; out["held_bad"] = c.heldBad;
    out["frames"] = frames;
    return out;
}

}  // namespace cascade::engine::wireb
