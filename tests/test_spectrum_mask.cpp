// Tests for core/spectrum_mask.hpp - the mask file, the evaluator, the breach monitor and the capture trigger.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/spectrum_mask.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "test_check.hpp"

using namespace cascade::core;

namespace {

Mask parseOk(const std::string& t) {
    Mask m;
    const MaskParseResult r = parseMask(t, m);
    CHECK(r.ok());
    return m;
}
MaskParseResult parseErr(const std::string& t) {
    Mask m;
    const MaskParseResult r = parseMask(t, m);
    CHECK(!r.ok());
    CHECK(m.segments.empty());
    return r;
}

void testParse() {
    std::printf("  parse: segments, comments, CRLF, case of the keywords, serialize round trip\n");
    const Mask m = parseOk("# a mask\r\nfoxmask 1  # version\r\n\r\nupper 99e6 -30 101e6 -30\r\nlower 99.5e6 -90 100.5e6 -80 # floor\n");
    CHECK(m.segments.size() == 2u);
    CHECK(m.segments[0].kind == MaskKind::Upper && m.segments[0].f1 == 99e6 && m.segments[0].l2 == -30.0);
    CHECK(m.segments[1].kind == MaskKind::Lower && m.segments[1].l1 == -90.0 && m.segments[1].l2 == -80.0);
    Mask back;
    CHECK(parseMask(serializeMask(m), back).ok());
    CHECK(back.segments.size() == m.segments.size());
    for (std::size_t i = 0; i < m.segments.size() && i < back.segments.size(); ++i) {
        CHECK(back.segments[i].kind == m.segments[i].kind);
        CHECK(back.segments[i].f1 == m.segments[i].f1 && back.segments[i].f2 == m.segments[i].f2);
        CHECK(back.segments[i].l1 == m.segments[i].l1 && back.segments[i].l2 == m.segments[i].l2);
    }
    CHECK(serializeMask(m).rfind("foxmask 1\n", 0) == 0);
    // No trailing newline on the last line.
    CHECK(parseMask(std::string("foxmask 1\nupper 1 0 2 0"), back).ok());
}

void testParseErrors() {
    std::printf("  parse errors: each code, with its line\n");
    CHECK(parseErr("upper 1 0 2 0\n").error == MaskError::NoHeader);
    CHECK(parseErr("").error == MaskError::NoHeader);
    CHECK(parseErr("# only a comment\n").error == MaskError::NoHeader);
    CHECK(parseErr("foxmask\nupper 1 0 2 0\n").error == MaskError::BadVersion);
    CHECK(parseErr("foxmask 2\nupper 1 0 2 0\n").error == MaskError::BadVersion);
    CHECK(parseErr("foxmask 1 extra\nupper 1 0 2 0\n").error == MaskError::BadVersion);
    MaskParseResult r = parseErr("foxmask 1\nupper 1 0 2 0\nmiddle 1 0 2 0\n");
    CHECK(r.error == MaskError::UnknownKeyword && r.line == 3u);
    r = parseErr("foxmask 1\nupper 1 0 2\n");
    CHECK(r.error == MaskError::BadNumber && r.line == 2u);
    CHECK(parseErr("foxmask 1\nupper 1 0 2 0 9\n").error == MaskError::BadNumber);          // trailing junk
    CHECK(parseErr("foxmask 1\nupper 1 0 2 nan\n").error == MaskError::BadNumber);
    CHECK(parseErr("foxmask 1\nupper 1 0 inf 0\n").error == MaskError::BadNumber);
    CHECK(parseErr("foxmask 1\nupper 0 0 2 0\n").error == MaskError::BadNumber);            // f must be > 0
    CHECK(parseErr("foxmask 1\nupper -5 0 2 0\n").error == MaskError::BadNumber);
    CHECK(parseErr("foxmask 1\nupper 1 0 2e14 0\n").error == MaskError::BadNumber);         // beyond 1e13
    CHECK(parseErr("foxmask 1\nupper 1 401 2 0\n").error == MaskError::BadNumber);          // level beyond 400 dB
    CHECK(parseErr("foxmask 1\nupper 1 0 2 -401\n").error == MaskError::BadNumber);
    CHECK(parseErr("foxmask 1\nupper 1 abc 2 0\n").error == MaskError::BadNumber);
    CHECK(parseErr("foxmask 1\nupper 1 0x10 2 0\n").error == MaskError::BadNumber);         // hex is not a number here
    CHECK(parseErr("foxmask 1\nupper 2 0 1 0\n").error == MaskError::BadOrder);
    CHECK(parseErr("foxmask 1\nupper 2 0 2 0\n").error == MaskError::BadOrder);
    CHECK(parseErr("foxmask 1\n# nothing\n").error == MaskError::NoSegments);
    // Size limits.
    std::string many = "foxmask 1\n";
    for (std::size_t i = 0; i < kMaskMaxSegments; ++i) { many += "upper 1 0 2 0\n"; }
    CHECK(parseOk(many).segments.size() == kMaskMaxSegments);
    many += "upper 1 0 2 0\n";
    CHECK(parseErr(many).error == MaskError::TooManySegments);
    CHECK(parseErr("foxmask 1\n" + std::string(kMaskMaxLine + 1, '#') + "\n").error == MaskError::LineTooLong);
    CHECK(parseOk("foxmask 1\nupper 1 0 2 0 " + std::string(kMaskMaxLine - 24 - 2, ' ') + "#\n").segments.size() == 1u);   // exactly at the line limit
    CHECK(parseErr(std::string(kMaskMaxBytes + 1, '\n')).error == MaskError::TooLarge);
    Mask m;
    CHECK(parseMask(nullptr, 5, m).error == MaskError::NoHeader);
    CHECK(!parseMask(nullptr, 0, m).ok());
    CHECK(maskErrorText(MaskError::BadOrder) != nullptr);
}

std::vector<float> flat(std::size_t n, float v) { return std::vector<float>(n, v); }

void testEvaluator() {
    std::printf("  evaluator: pass/fail, worst margin and bin, slopes, overlap, lower limits, no limit, bad bins\n");
    const std::size_t n = 1000;
    const double centre = 100e6, span = 2e6, binHz = span / n;
    MaskEvaluator ev;
    CHECK(!ev.valid());
    {
        const Mask m = parseOk("foxmask 1\nupper 99e6 -30 101e6 -30\n");
        CHECK(ev.prepare(m, centre, span, n));
        std::vector<float> f = flat(n, -50.0f);
        MaskResult r;
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(r.pass && r.anyLimited && r.breachBins == 0u);
        CHECK_NEAR(r.worstMarginDb, 20.0, 1e-9);
        f[700] = -20.0f;                                           // 10 dB over, at 100.4 MHz
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(!r.pass && r.breachBins == 1u && r.worstBin == 700u);
        CHECK_NEAR(r.worstMarginDb, -10.0, 1e-9);
        CHECK_NEAR(r.worstFreqHz, centre + 200.0 * binHz, 1e-3);
        CHECK(r.worstKind == MaskKind::Upper);
        f[700] = -30.0f;                                           // exactly on the line passes
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(r.pass && r.breachBins == 0u);
        CHECK_NEAR(r.worstMarginDb, 0.0, 1e-9);
        // Edge bins at 99.0 MHz (bin 0 is at 99.0 MHz exactly) are on the segment end: limited.
        CHECK_NEAR(ev.upperLimit(0), -30.0, 1e-12);
        CHECK(std::isnan(ev.lowerLimit(0)));
        CHECK(std::isnan(ev.upperLimit(n + 5)));
    }
    {   // A slope: -30 dB at 99 MHz to -50 dB at 101 MHz; the limit at bin i is linear.
        const Mask m = parseOk("foxmask 1\nupper 99e6 -30 101e6 -50\n");
        CHECK(ev.prepare(m, centre, span, n));
        CHECK_NEAR(ev.upperLimit(500), -40.0, 1e-9);               // the centre bin is at 100 MHz
        CHECK_NEAR(ev.upperLimit(250), -35.0, 1e-9);
        std::vector<float> f = flat(n, -60.0f);
        f[250] = -34.0f;
        MaskResult r;
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(!r.pass && r.worstBin == 250u);
        CHECK_NEAR(r.worstMarginDb, -1.0, 1e-9);
    }
    {   // Overlap: the lower upper line wins; lower limit: the higher wins.
        const Mask m = parseOk("foxmask 1\nupper 99e6 -30 101e6 -30\nupper 100e6 -45 100.2e6 -45\nlower 99e6 -90 101e6 -90\nlower 99.9e6 -70 100.1e6 -70\n");
        CHECK(ev.prepare(m, centre, span, n));
        CHECK_NEAR(ev.upperLimit(500), -45.0, 1e-12);
        CHECK_NEAR(ev.upperLimit(100), -30.0, 1e-12);
        CHECK_NEAR(ev.lowerLimit(500), -70.0, 1e-12);
        CHECK_NEAR(ev.lowerLimit(100), -90.0, 1e-12);
        std::vector<float> f = flat(n, -60.0f);                    // inside both
        f[500] = -75.0f;                                           // under the raised floor
        MaskResult r;
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(!r.pass && r.worstKind == MaskKind::Lower && r.worstBin == 500u);
        CHECK_NEAR(r.worstMarginDb, -5.0, 1e-9);
    }
    {   // A mask wholly outside the frame limits nothing.
        const Mask m = parseOk("foxmask 1\nupper 200e6 -30 201e6 -30\n");
        CHECK(ev.prepare(m, centre, span, n));
        std::vector<float> f = flat(n, 0.0f);
        MaskResult r;
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(r.pass && !r.anyLimited);
        CHECK(std::isinf(r.worstMarginDb));
    }
    {   // Segment narrower than a bin that contains no bin centre: nothing limited.
        const Mask m = parseOk("foxmask 1\nupper 100.0001e6 -30 100.0002e6 -30\n");
        CHECK(ev.prepare(m, centre, span, n));
        std::vector<float> f = flat(n, 0.0f);
        MaskResult r;
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(r.pass && !r.anyLimited);
    }
    {   // Non-finite bins are skipped and counted; a wrong frame length is refused.
        const Mask m = parseOk("foxmask 1\nupper 99e6 -30 101e6 -30\n");
        CHECK(ev.prepare(m, centre, span, n));
        std::vector<float> f = flat(n, -50.0f);
        f[10] = std::nanf("");
        f[11] = std::numeric_limits<float>::infinity();
        MaskResult r;
        CHECK(ev.evaluate(f.data(), n, r));
        CHECK(r.pass && r.invalidBins == 2u);
        MaskResult untouched;
        untouched.breachBins = 77;
        CHECK(!ev.evaluate(f.data(), n - 1, untouched));
        CHECK(untouched.breachBins == 77u);
        CHECK(!ev.evaluate(nullptr, n, untouched));
    }
    {   // prepare() refusals.
        const Mask m = parseOk("foxmask 1\nupper 99e6 -30 101e6 -30\n");
        CHECK(!ev.prepare(Mask{}, centre, span, n));
        CHECK(!ev.valid());
        CHECK(!ev.prepare(m, centre, 0.0, n));
        CHECK(!ev.prepare(m, centre, -1.0, n));
        CHECK(!ev.prepare(m, std::nan(""), span, n));
        CHECK(!ev.prepare(m, centre, span, 1));
        CHECK(!ev.prepare(m, centre, span, kMaskMaxBins + 1));
    }
}

// Builds a MaskResult for the monitor tests.
MaskResult res(double margin, double f = 433.92e6, MaskKind k = MaskKind::Upper) {
    MaskResult r;
    r.anyLimited = true;
    r.worstMarginDb = margin;
    r.worstFreqHz = f;
    r.worstKind = k;
    r.pass = !(margin < 0.0);
    r.breachBins = r.pass ? 0 : 1;
    return r;
}

void testMonitor() {
    std::printf("  monitor: debounce up and down, worst margin, trigger once with holdoff, bounded events, log line\n");
    MaskMonitor mon;
    MaskMonitorConfig c;
    c.raiseFrames = 3;
    c.clearFrames = 5;
    c.minTriggerIntervalS = 10.0;
    mon.configure(c);
    double t = 0.0;
    auto step = [&](const MaskResult& r) { mon.update(r, t); t += 0.1; };
    step(res(-3.0));
    step(res(-3.0));
    CHECK(!mon.alarm() && mon.events().empty());
    step(res(5.0));                                              // a run shorter than the debounce is forgotten
    step(res(-3.0));
    step(res(-3.0));
    CHECK(!mon.alarm());
    step(res(-4.0, 434.0e6));                                     // third consecutive failing frame
    CHECK(mon.alarm());
    CHECK(mon.events().size() == 1u);
    CHECK(mon.events().back().open);
    CHECK_NEAR(mon.events().back().worstMarginDb, -4.0, 1e-12);
    CHECK_NEAR(mon.events().back().worstFreqHz, 434.0e6, 1e-3);
    CHECK(mon.events().back().frames == 3u);
    CHECK_NEAR(mon.events().back().startS, 0.3, 1e-9);            // the first failing frame of THIS run
    CaptureTrigger tr = mon.takeTrigger();
    CHECK(tr.pending);
    CHECK_NEAR(tr.freqHz, 434.0e6, 1e-3);
    CHECK_NEAR(tr.marginDb, -4.0, 1e-12);
    CHECK_NEAR(tr.preRollS, 0.5, 1e-12);
    CHECK_NEAR(tr.postRollS, 2.0, 1e-12);
    CHECK(!mon.takeTrigger().pending);                            // handed over once
    step(res(-9.0, 435.0e6, MaskKind::Lower));                    // worse: the event follows it
    CHECK_NEAR(mon.events().back().worstMarginDb, -9.0, 1e-12);
    CHECK(mon.events().back().kind == MaskKind::Lower);
    CHECK(mon.events().back().frames == 4u);
    // Flicker inside the alarm does not clear it; five passing frames in a row do.
    step(res(5.0));
    step(res(5.0));
    step(res(-1.0));
    CHECK(mon.alarm());
    for (int i = 0; i < 4; ++i) { step(res(5.0)); }
    CHECK(mon.alarm());
    step(res(5.0));
    CHECK(!mon.alarm());
    CHECK(!mon.events().back().open);
    // A second breach within the hold-off is an event but no new trigger; after it, a trigger again.
    for (int i = 0; i < 3; ++i) { step(res(-2.0)); }
    CHECK(mon.alarm() && mon.events().size() == 2u);
    CHECK(!mon.takeTrigger().pending);
    for (int i = 0; i < 5; ++i) { step(res(5.0)); }
    t += 20.0;
    for (int i = 0; i < 3; ++i) { step(res(-2.0)); }
    CHECK(mon.alarm() && mon.events().size() == 3u);
    CHECK(mon.takeTrigger().pending);
    // A frame with no limited bin counts as passing.
    MaskResult none;
    none.anyLimited = false;
    for (int i = 0; i < 5; ++i) { step(none); }
    CHECK(!mon.alarm());
    // Bounded event list.
    mon.configure(c);
    c.minTriggerIntervalS = 0.0;
    mon.configure(c);
    for (int k = 0; k < static_cast<int>(kMaskMaxEvents) + 20; ++k) {
        for (int i = 0; i < 3; ++i) { step(res(-1.0 - k)); }
        for (int i = 0; i < 5; ++i) { step(res(5.0)); }
    }
    CHECK(mon.events().size() == kMaskMaxEvents);
    CHECK(mon.droppedEvents() == 20u);
    CHECK_NEAR(mon.events().back().worstMarginDb, -1.0 - (kMaskMaxEvents + 19), 1e-9);
    // Degenerate configuration is repaired.
    MaskMonitorConfig z;
    z.raiseFrames = 0;
    z.clearFrames = 0;
    z.minTriggerIntervalS = -4.0;
    z.preRollS = std::nan("");
    mon.configure(z);
    step(res(-1.0));
    CHECK(mon.alarm());
    CHECK(mon.takeTrigger().pending);
    step(res(1.0));
    CHECK(!mon.alarm());
}

void testLogLine() {
    std::printf("  log line: bounded, only kind / margin / frequency / duration\n");
    BreachEvent e;
    e.worstMarginDb = -3.24;
    e.worstFreqHz = 433.92e6;
    e.frames = 12;
    e.kind = MaskKind::Upper;
    std::string s = formatBreachLine(e);
    CHECK(s == "mask breach: upper margin -3.2 dB at 433.920000 MHz, 12 frames (open)");
    e.open = false;
    e.kind = MaskKind::Lower;
    s = formatBreachLine(e);
    CHECK(s.find("lower") != std::string::npos && s.find("(open)") == std::string::npos);
    e.worstMarginDb = -1e300;
    e.worstFreqHz = 1e300;
    e.frames = static_cast<std::size_t>(-1);
    CHECK(formatBreachLine(e).size() < kMaskLogLineMax);
}

void testFileLoader() {
    std::printf("  file loader: reads a file, refuses a missing one and an oversized one\n");
    namespace fs = std::filesystem;
    const std::string stem = "rfmon-mask-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path ok = fs::path(stem + "-ok.tmp");
    const fs::path big = fs::path(stem + "-big.tmp");
    {
        std::ofstream f(ok, std::ios::binary);
        f << "foxmask 1\nupper 1e6 -10 2e6 -10\n";
    }
    {
        std::ofstream f(big, std::ios::binary);
        f << std::string(kMaskMaxBytes + 10, ' ');
    }
    Mask m;
    CHECK(loadMaskFile(ok.string(), m).ok() && m.segments.size() == 1u);
    CHECK(loadMaskFile(big.string(), m).error == MaskError::TooLarge);
    CHECK(m.segments.empty());
    CHECK(loadMaskFile(stem + "-missing.tmp", m).error == MaskError::Unreadable);
    std::error_code ec;
    fs::remove(ok, ec);
    fs::remove(big, ec);
}

void testFuzz() {
    std::printf("  fuzz: random bytes and mutated masks never crash, and whatever parses holds the invariants\n");
    std::mt19937 g(77);
    const std::string seed =
        "# mask\nfoxmask 1\nupper 99e6 -30 101e6 -30\nlower 99.5e6 -90 100.5e6 -80\nupper 1.5e9 0 2e9 -10\n";
    std::size_t parsed = 0, refused = 0;
    auto check = [&](const std::string& s) {
        Mask m;
        const MaskParseResult r = parseMask(s, m);
        if (r.ok()) {
            ++parsed;
            CHECK(!m.segments.empty() && m.segments.size() <= kMaskMaxSegments);
            for (const MaskSegment& sg : m.segments) {
                CHECK(std::isfinite(sg.f1) && std::isfinite(sg.f2) && sg.f1 > 0.0 && sg.f1 < sg.f2 && sg.f2 <= kMaskMaxFreqHz);
                CHECK(std::fabs(sg.l1) <= kMaskMaxLevelDb && std::fabs(sg.l2) <= kMaskMaxLevelDb);
            }
            Mask back;
            CHECK(parseMask(serializeMask(m), back).ok());
            CHECK(back.segments.size() == m.segments.size());
            MaskEvaluator ev;
            if (ev.prepare(m, 100e6, 2e6, 256)) {
                std::vector<float> f(256, -50.0f);
                MaskResult res;
                CHECK(ev.evaluate(f.data(), 256, res));
            }
        } else {
            ++refused;
            CHECK(m.segments.empty());
            CHECK(r.error != MaskError::None);
        }
    };
    for (int it = 0; it < 20000; ++it) {
        std::string s;
        const int kind = it % 3;
        if (kind == 0) {                                          // random bytes
            const std::size_t len = g() % 300;
            for (std::size_t i = 0; i < len; ++i) { s.push_back(static_cast<char>(g() & 0xFF)); }
        } else {                                                  // a valid mask, mutated
            s = seed;
            const int muts = 1 + static_cast<int>(g() % 4);
            for (int k = 0; k < muts && !s.empty(); ++k) {
                const std::size_t at = g() % s.size();
                switch (g() % 4) {
                    case 0: s[at] = static_cast<char>(g() & 0xFF); break;
                    case 1: s.erase(at, 1 + g() % 6); break;
                    case 2: s.insert(at, 1, "0123456789-+.eE \t#\n"[g() % 19]); break;
                    default: s.resize(at); break;
                }
            }
        }
        check(s);
    }
    CHECK(parsed > 100u);        // the mutations leave plenty valid ...
    CHECK(refused > 1000u);      // ... and plenty refused: both paths were exercised
}

}  // namespace

int main() {
    testParse();
    testParseErrors();
    testEvaluator();
    testMonitor();
    testLogLine();
    testFileLoader();
    testFuzz();
    return testSummary("test_spectrum_mask");
}
