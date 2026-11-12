// spectrum_mask.hpp - LIMIT LINES / FREQUENCY MASK for a spectrum trace: a versioned mask file, a per-frame pass/fail evaluator with the worst
// margin and the offending bin, a breach monitor with debounce (alarm, bounded log line, bounded event list) and the trigger record a
// recorder can act on ("capture I/Q on breach").
//
// STAND-ALONE (lane rfmon): nothing in the engine uses it. The capture itself is a hand-off: this header only says WHEN (a CaptureTrigger).
// The Survey Engine version of a mask belongs to the survey-maps plugin lane and shares no code with this file.
//
// THE FILE (text, version 1). First significant line `foxmask 1`; then any number of
//     upper <f1_hz> <level1_db> <f2_hz> <level2_db>      the trace must stay AT OR BELOW the line from f1 to f2
//     lower <f1_hz> <level1_db> <f2_hz> <level2_db>      the trace must stay AT OR ABOVE the line from f1 to f2
// one segment per line, level straight in dB against LINEAR frequency between the ends, f1 < f2. Where several upper segments cover a
// frequency the lowest line limits it, where several lower segments do the highest does; where none covers it nothing limits it. Blank
// lines and `#` comments are ignored. Limits: file <= 64 KiB, line <= 200 bytes, <= 512 segments, frequency in (0, 1e13] Hz, level in
// [-400, 400] dB, finite numbers only. A mask holds no name and no identifier of anything.
//
// MARGIN. For an upper limit margin = limit - level; for a lower limit margin = level - limit. Margin < 0 is a breach, margin == 0 passes.
// The level unit is whatever the trace is in (the mask and the trace must agree; the block does not convert).
//
// FRAMES. n values, bin 0 the LOWEST frequency, bin n/2 at the centre frequency (DC in the middle): f_i = centre + (i - n/2) * span / n.
// Non-finite level bins are skipped and counted (invalidBins); a frame with no limited bin at all passes with anyLimited = false.
//
// THREADS: nothing locks. prepare() allocates (control thread); evaluate()/update() do not allocate except that the bounded event list
// grows to its cap once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace cascade::core {

inline constexpr std::size_t kMaskMaxBytes = 64 * 1024;
inline constexpr std::size_t kMaskMaxLine = 200;
inline constexpr std::size_t kMaskMaxSegments = 512;
inline constexpr double kMaskMaxFreqHz = 1e13;
inline constexpr double kMaskMaxLevelDb = 400.0;
inline constexpr std::size_t kMaskMaxBins = 1u << 20;
inline constexpr std::size_t kMaskMaxEvents = 64;
inline constexpr std::size_t kMaskLogLineMax = 120;

enum class MaskKind { Upper, Lower };

struct MaskSegment {
    MaskKind kind = MaskKind::Upper;
    double f1 = 0.0, l1 = 0.0, f2 = 0.0, l2 = 0.0;
};

struct Mask {
    std::vector<MaskSegment> segments;
};

enum class MaskError {
    None,
    TooLarge,          // file over kMaskMaxBytes
    LineTooLong,
    NoHeader,          // first significant line is not `foxmask <version>`
    BadVersion,
    UnknownKeyword,
    BadNumber,         // missing, malformed, non-finite or out of range
    BadOrder,          // f1 >= f2
    TooManySegments,
    NoSegments,        // a header and nothing to check
    Unreadable,        // file could not be opened
};

struct MaskParseResult {
    MaskError error = MaskError::None;
    std::size_t line = 0;   // 1-based line of the fault; 0 when none or not line-bound
    bool ok() const { return error == MaskError::None; }
};

inline const char* maskErrorText(MaskError e) {
    switch (e) {
        case MaskError::None: return "ok";
        case MaskError::TooLarge: return "mask file too large";
        case MaskError::LineTooLong: return "mask line too long";
        case MaskError::NoHeader: return "mask file must start with 'foxmask 1'";
        case MaskError::BadVersion: return "mask file version not supported";
        case MaskError::UnknownKeyword: return "unknown keyword in mask file";
        case MaskError::BadNumber: return "bad number in mask file";
        case MaskError::BadOrder: return "segment frequencies must increase";
        case MaskError::TooManySegments: return "too many segments in mask file";
        case MaskError::NoSegments: return "mask file has no segments";
        case MaskError::Unreadable: return "mask file could not be read";
    }
    return "?";
}

namespace mask_detail {

inline bool parseNumber(const char*& p, const char* end, double lo, double hi, bool loExclusive, double& out) {
    while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
    if (p >= end) { return false; }
    const char* s = p;
    while (p < end && *p != ' ' && *p != '\t') { ++p; }
    char buf[48];
    const std::size_t len = static_cast<std::size_t>(p - s);
    if (len == 0 || len >= sizeof(buf)) { return false; }
    std::memcpy(buf, s, len);
    buf[len] = '\0';
    // Decimal notation only: strtod would also take hex floats ("0x10"), "inf" and "nan".
    for (std::size_t i = 0; i < len; ++i) {
        const char ch = buf[i];
        if (!((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.' || ch == 'e' || ch == 'E')) { return false; }
    }
    char* e = nullptr;
    const double v = std::strtod(buf, &e);
    if (e == buf || *e != '\0' || !std::isfinite(v)) { return false; }
    if (loExclusive ? !(v > lo) : !(v >= lo)) { return false; }
    if (v > hi) { return false; }
    out = v;
    return true;
}

}  // namespace mask_detail

// Parses a mask from memory. On failure `out` is left empty.
inline MaskParseResult parseMask(const char* text, std::size_t len, Mask& out) {
    out.segments.clear();
    MaskParseResult r;
    if (text == nullptr && len != 0) { r.error = MaskError::NoHeader; return r; }
    if (len > kMaskMaxBytes) { r.error = MaskError::TooLarge; return r; }
    Mask m;
    bool header = false;
    std::size_t pos = 0, lineNo = 0;
    while (pos < len) {
        std::size_t eol = pos;
        while (eol < len && text[eol] != '\n') { ++eol; }
        ++lineNo;
        std::size_t a = pos, b = eol;
        pos = eol + 1;
        if (b > a && text[b - 1] == '\r') { --b; }
        if (b - a > kMaskMaxLine) { r.error = MaskError::LineTooLong; r.line = lineNo; return r; }
        const char* p = text + a;
        const char* end = text + b;
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
        if (p >= end || *p == '#') { continue; }
        const char* w = p;
        while (p < end && *p != ' ' && *p != '\t') { ++p; }
        const std::size_t wl = static_cast<std::size_t>(p - w);
        if (!header) {
            if (!(wl == 7 && std::memcmp(w, "foxmask", 7) == 0)) { r.error = MaskError::NoHeader; r.line = lineNo; return r; }
            double ver = 0;
            if (!mask_detail::parseNumber(p, end, 0.0, 1e6, false, ver)) { r.error = MaskError::BadVersion; r.line = lineNo; return r; }
            if (ver != 1.0) { r.error = MaskError::BadVersion; r.line = lineNo; return r; }
            while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
            if (p < end && *p != '#') { r.error = MaskError::BadVersion; r.line = lineNo; return r; }
            header = true;
            continue;
        }
        MaskSegment s;
        if (wl == 5 && std::memcmp(w, "upper", 5) == 0) { s.kind = MaskKind::Upper; }
        else if (wl == 5 && std::memcmp(w, "lower", 5) == 0) { s.kind = MaskKind::Lower; }
        else { r.error = MaskError::UnknownKeyword; r.line = lineNo; return r; }
        if (!mask_detail::parseNumber(p, end, 0.0, kMaskMaxFreqHz, true, s.f1) ||
            !mask_detail::parseNumber(p, end, -kMaskMaxLevelDb, kMaskMaxLevelDb, false, s.l1) ||
            !mask_detail::parseNumber(p, end, 0.0, kMaskMaxFreqHz, true, s.f2) ||
            !mask_detail::parseNumber(p, end, -kMaskMaxLevelDb, kMaskMaxLevelDb, false, s.l2)) {
            r.error = MaskError::BadNumber;
            r.line = lineNo;
            return r;
        }
        while (p < end && (*p == ' ' || *p == '\t')) { ++p; }
        if (p < end && *p != '#') { r.error = MaskError::BadNumber; r.line = lineNo; return r; }   // trailing junk
        if (!(s.f1 < s.f2)) { r.error = MaskError::BadOrder; r.line = lineNo; return r; }
        if (m.segments.size() >= kMaskMaxSegments) { r.error = MaskError::TooManySegments; r.line = lineNo; return r; }
        m.segments.push_back(s);
    }
    if (!header) { r.error = MaskError::NoHeader; return r; }
    if (m.segments.empty()) { r.error = MaskError::NoSegments; return r; }
    out = std::move(m);
    return r;
}
inline MaskParseResult parseMask(const std::string& text, Mask& out) { return parseMask(text.data(), text.size(), out); }

// Reads a bounded file. The file is read whole (at most kMaskMaxBytes + 1 bytes are taken).
inline MaskParseResult loadMaskFile(const std::string& path, Mask& out) {
    out.segments.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) { MaskParseResult r; r.error = MaskError::Unreadable; return r; }
    std::string buf(kMaskMaxBytes + 1, '\0');
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return parseMask(buf, out);
}

// The file's text for a mask (versioned, ASCII, numbers with enough digits to round-trip).
inline std::string serializeMask(const Mask& m) {
    std::string s = "foxmask 1\n";
    char line[160];
    for (const MaskSegment& g : m.segments) {
        std::snprintf(line, sizeof(line), "%s %.17g %.17g %.17g %.17g\n", g.kind == MaskKind::Upper ? "upper" : "lower", g.f1, g.l1, g.f2, g.l2);
        s += line;
    }
    return s;
}

// ----- Evaluation -----------------------------------------------------------------------------------------------------------------------

struct MaskResult {
    bool pass = true;
    bool anyLimited = false;          // at least one bin is under a limit
    double worstMarginDb = std::numeric_limits<double>::infinity();   // smallest margin over all limited bins (negative: breach)
    std::size_t worstBin = 0;
    double worstFreqHz = 0.0;
    MaskKind worstKind = MaskKind::Upper;
    std::size_t breachBins = 0;
    std::size_t invalidBins = 0;      // non-finite level bins that were skipped
};

class MaskEvaluator {
public:
    // Computes the limit at every bin of a frame once (control thread). False on unusable arguments or an empty mask.
    bool prepare(const Mask& m, double centreHz, double spanHz, std::size_t bins) {
        valid_ = false;
        if (m.segments.empty() || bins < 2 || bins > kMaskMaxBins || !(spanHz > 0.0) || !std::isfinite(spanHz) || !std::isfinite(centreHz)) { return false; }
        centre_ = centreHz;
        span_ = spanHz;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        upper_.assign(bins, nan);
        lower_.assign(bins, nan);
        const double binHz = spanHz / static_cast<double>(bins);
        const double fLo = centreHz - 0.5 * spanHz;
        const double fHi = fLo + spanHz;
        for (const MaskSegment& g : m.segments) {
            if (g.f2 < fLo || g.f1 > fHi) { continue; }
            // bins whose centre lies in [f1, f2]
            const double i0 = std::ceil((g.f1 - centreHz) / binHz + static_cast<double>(bins) / 2.0);
            const double i1 = std::floor((g.f2 - centreHz) / binHz + static_cast<double>(bins) / 2.0);
            const std::size_t a = static_cast<std::size_t>(std::max(0.0, i0));
            const double i1c = std::min(static_cast<double>(bins) - 1.0, i1);
            if (i1c < 0.0) { continue; }
            const std::size_t b = static_cast<std::size_t>(i1c);
            for (std::size_t i = a; i <= b && i < bins; ++i) {
                const double f = centreHz + (static_cast<double>(i) - static_cast<double>(bins) / 2.0) * binHz;
                if (f < g.f1 || f > g.f2) { continue; }
                const double t = (f - g.f1) / (g.f2 - g.f1);
                const double lim = g.l1 + t * (g.l2 - g.l1);
                double& slot = g.kind == MaskKind::Upper ? upper_[i] : lower_[i];
                if (std::isnan(slot)) { slot = lim; }
                else { slot = g.kind == MaskKind::Upper ? std::min(slot, lim) : std::max(slot, lim); }
            }
        }
        valid_ = true;
        return true;
    }
    bool valid() const { return valid_; }
    std::size_t bins() const { return upper_.size(); }
    double centreHz() const { return centre_; }
    double spanHz() const { return span_; }
    // The limits at a bin (NaN: none), for drawing the lines.
    double upperLimit(std::size_t i) const { return i < upper_.size() ? upper_[i] : std::numeric_limits<double>::quiet_NaN(); }
    double lowerLimit(std::size_t i) const { return i < lower_.size() ? lower_[i] : std::numeric_limits<double>::quiet_NaN(); }

    // One frame; a frame of another length is not evaluated (returns false and leaves `r` untouched).
    bool evaluate(const float* level, std::size_t n, MaskResult& r) const {
        if (!valid_ || level == nullptr || n != upper_.size()) { return false; }
        MaskResult o;
        const double binHz = span_ / static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double v = level[i];
            if (!std::isfinite(v)) { ++o.invalidBins; continue; }
            for (int k = 0; k < 2; ++k) {
                const double lim = k == 0 ? upper_[i] : lower_[i];
                if (std::isnan(lim)) { continue; }
                o.anyLimited = true;
                const double margin = k == 0 ? lim - v : v - lim;
                if (margin < 0.0) { ++o.breachBins; }
                if (margin < o.worstMarginDb) {
                    o.worstMarginDb = margin;
                    o.worstBin = i;
                    o.worstKind = k == 0 ? MaskKind::Upper : MaskKind::Lower;
                    o.worstFreqHz = centre_ + (static_cast<double>(i) - static_cast<double>(n) / 2.0) * binHz;
                }
            }
        }
        o.pass = !(o.worstMarginDb < 0.0);
        r = o;
        return true;
    }

private:
    bool valid_ = false;
    double centre_ = 0.0, span_ = 0.0;
    std::vector<double> upper_, lower_;
};

// ----- Breach monitor ------------------------------------------------------------------------------------------------------------------

struct MaskMonitorConfig {
    std::size_t raiseFrames = 3;          // consecutive failing frames before the alarm is raised (>= 1)
    std::size_t clearFrames = 5;          // consecutive passing frames before it is cleared (>= 1)
    double minTriggerIntervalS = 10.0;    // at least this long between two capture triggers
    double preRollS = 0.5;                // what the trigger asks the recorder to keep from before the breach
    double postRollS = 2.0;               // ... and to keep recording after it
};

struct BreachEvent {
    double startS = 0.0;
    double endS = 0.0;                    // last frame time seen while the alarm was up
    std::size_t frames = 0;               // frames the breach lasted (from the first failing frame of the run)
    double worstMarginDb = 0.0;
    double worstFreqHz = 0.0;
    MaskKind kind = MaskKind::Upper;
    bool open = true;                     // still in progress
};

// The record a recorder acts on. The capture is theirs; this says when and why.
struct CaptureTrigger {
    bool pending = false;
    double atS = 0.0;
    double preRollS = 0.0;
    double postRollS = 0.0;
    double freqHz = 0.0;                  // where the worst bin was when the alarm rose
    double marginDb = 0.0;
    MaskKind kind = MaskKind::Upper;
};

// One bounded line, no identifiers: only the kind of limit, the margin, a frequency and a duration.
inline std::string formatBreachLine(const BreachEvent& e) {
    char b[kMaskLogLineMax];
    const double mhz = e.worstFreqHz / 1e6;
    std::snprintf(b, sizeof(b), "mask breach: %s margin %.1f dB at %.6f MHz, %zu frames%s", e.kind == MaskKind::Upper ? "upper" : "lower", e.worstMarginDb, mhz,
                  e.frames, e.open ? " (open)" : "");
    return std::string(b);
}

class MaskMonitor {
public:
    void configure(const MaskMonitorConfig& c) {
        cfg_ = c;
        if (cfg_.raiseFrames < 1) { cfg_.raiseFrames = 1; }
        if (cfg_.clearFrames < 1) { cfg_.clearFrames = 1; }
        if (!(cfg_.minTriggerIntervalS >= 0.0)) { cfg_.minTriggerIntervalS = 0.0; }
        if (!(cfg_.preRollS >= 0.0)) { cfg_.preRollS = 0.0; }
        if (!(cfg_.postRollS >= 0.0)) { cfg_.postRollS = 0.0; }
        reset();
    }
    void reset() {
        failRun_ = passRun_ = 0;
        alarm_ = false;
        events_.clear();
        trigger_ = CaptureTrigger{};
        haveLastTrigger_ = false;
        runWorst_ = MaskResult{};
        runStartS_ = 0.0;
        runFrames_ = 0;
    }

    bool alarm() const { return alarm_; }
    const std::deque<BreachEvent>& events() const { return events_; }   // newest last; at most kMaskMaxEvents (the oldest drop off)
    std::size_t droppedEvents() const { return dropped_; }

    // One evaluated frame at time `nowS` (a monotonic clock). A frame that was not evaluated (no limited bin) counts as passing.
    void update(const MaskResult& r, double nowS) {
        const bool failing = r.anyLimited && !r.pass;
        if (failing) {
            ++failRun_;
            passRun_ = 0;
            if (failRun_ == 1) { runStartS_ = nowS; runFrames_ = 0; runWorst_ = r; }
            ++runFrames_;
            if (r.worstMarginDb < runWorst_.worstMarginDb) { runWorst_ = r; }
            if (!alarm_ && failRun_ >= cfg_.raiseFrames) {
                alarm_ = true;
                BreachEvent e;
                e.startS = runStartS_;
                e.endS = nowS;
                e.frames = runFrames_;
                e.worstMarginDb = runWorst_.worstMarginDb;
                e.worstFreqHz = runWorst_.worstFreqHz;
                e.kind = runWorst_.worstKind;
                push(e);
                if (!haveLastTrigger_ || nowS - lastTriggerS_ >= cfg_.minTriggerIntervalS) {
                    trigger_.pending = true;
                    trigger_.atS = nowS;
                    trigger_.preRollS = cfg_.preRollS;
                    trigger_.postRollS = cfg_.postRollS;
                    trigger_.freqHz = e.worstFreqHz;
                    trigger_.marginDb = e.worstMarginDb;
                    trigger_.kind = e.kind;
                    lastTriggerS_ = nowS;
                    haveLastTrigger_ = true;
                }
            } else if (alarm_ && !events_.empty()) {
                BreachEvent& e = events_.back();
                e.endS = nowS;
                e.frames = runFrames_;
                if (r.worstMarginDb < e.worstMarginDb) {
                    e.worstMarginDb = r.worstMarginDb;
                    e.worstFreqHz = r.worstFreqHz;
                    e.kind = r.worstKind;
                }
            }
        } else {
            ++passRun_;
            // A run of failures shorter than the debounce is forgotten at once: it never was an alarm.
            if (!alarm_) { failRun_ = 0; }
            if (alarm_ && passRun_ >= cfg_.clearFrames) {
                alarm_ = false;
                failRun_ = 0;
                if (!events_.empty()) { events_.back().open = false; }
            }
        }
    }

    // Hands the pending capture trigger over once (pending = false afterwards).
    CaptureTrigger takeTrigger() {
        CaptureTrigger t = trigger_;
        trigger_.pending = false;
        return t;
    }

private:
    void push(const BreachEvent& e) {
        events_.push_back(e);
        while (events_.size() > kMaskMaxEvents) { events_.pop_front(); ++dropped_; }
    }
    MaskMonitorConfig cfg_{};
    std::size_t failRun_ = 0, passRun_ = 0;
    bool alarm_ = false;
    std::deque<BreachEvent> events_;
    std::size_t dropped_ = 0;
    CaptureTrigger trigger_{};
    double lastTriggerS_ = 0.0;
    bool haveLastTrigger_ = false;
    MaskResult runWorst_{};
    double runStartS_ = 0.0;
    std::size_t runFrames_ = 0;
};

}  // namespace cascade::core
