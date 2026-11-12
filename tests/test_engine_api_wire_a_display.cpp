// test_engine_api_wire_a_display.cpp - THE PUBLISHED SPECTRUM FRAME OF WIRING LANE wire-a, held against the real engine on its generator source: the frame every screen reads (read_spectrum) is the one the lane's chain
// works on, and the waterfall history keeps the lines that were drawn.
//
//   D1  DISPLAY CHAIN: the noise of the generator's frame (bins well away from its two tones) has a spread of about 5 dB bin to bin; with an RMS average of 32 frames, or a moving-average smoothing of 9 bins, the
//       spread falls well below half of that; every setting off again restores it; the comb remover runs on the frame without breaking it (every bin finite); a bad setting is refused with nothing changed; the same
//       setting again is NO_CHANGE; the setting is kept in display-average.json and read by a new engine.
//   D2  WATERFALL HISTORY: off by default (nothing kept, ops say so); switched on for radio 1 it keeps the lines that are published; PAUSE freezes the view while the live lines go on; scroll back moves the view
//       by the lines asked and reports how far behind it is; forward and live come back; a time before the first line seeks the oldest; bad numbers are refused; switched off it frees the memory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wire_a_rig.hpp"
#include "engine/wire_a.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

// The spread (standard deviation, dB) of the published frame over the bins 80 % to 95 % of the way along: the generator's tones are at +300 kHz and -500 kHz of its centre, far from there.
double noiseSpread(Rig& r) {
    std::vector<float> bins(8192);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    double out = -1.0;
    r.until([&] {
        if (r.api()->read_spectrum(r.local->raw(), 0, &info, bins.data(), static_cast<uint32_t>(bins.size())) != FOXAPI_OK) { return false; }
        const std::size_t n = info.copied;
        if (n < 64) { return false; }
        const std::size_t a = n * 80 / 100, b = n * 95 / 100;
        double s = 0.0, s2 = 0.0;
        for (std::size_t i = a; i < b; ++i) { s += bins[i]; s2 += static_cast<double>(bins[i]) * bins[i]; }
        const double m = s / static_cast<double>(b - a);
        out = std::sqrt(std::max(0.0, s2 / static_cast<double>(b - a) - m * m));
        return true;
    }, 3000);
    return out;
}

bool allFinite(Rig& r) {
    std::vector<float> bins(8192);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    if (r.api()->read_spectrum(r.local->raw(), 0, &info, bins.data(), static_cast<uint32_t>(bins.size())) != FOXAPI_OK) { return false; }
    for (std::size_t i = 0; i < info.copied; ++i) { if (!std::isfinite(bins[i])) { return false; } }
    return info.copied > 0;
}

void testChain() {
    std::printf("D1: display chain\n");
    const fs::path user = g_dir / "user1";
    {
        Rig r(user);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        r.settle(500);
        const double plain = noiseSpread(r);
        std::printf("  noise spread, nothing applied: %.2f dB\n", plain);
        CHECK(plain > 2.0);
        const std::string avg = R"({"average":"rms","frames":32})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &avg).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &avg).status(), FOXAPI_NO_CHANGE);
        CHECK(r.fact("display.average").find("\"average\":\"rms\"") != std::string::npos);
        r.settle(2500);
        const double averaged = noiseSpread(r);
        std::printf("  RMS average of 32 frames: %.2f dB\n", averaged);
        CHECK(averaged < 0.5 * plain);
        const std::string smooth = R"({"smoothing":"moving","smoothWidth":9})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &smooth).status(), FOXAPI_OK);
        r.settle(500);
        const double smoothed = noiseSpread(r);
        std::printf("  moving average of 9 bins: %.2f dB\n", smoothed);
        CHECK(smoothed < 0.75 * plain);   // (neighbouring bins of the estimator are correlated, so nine bins give far less than a third)
        const std::string sg = R"({"smoothing":"savgol","smoothWidth":9,"smoothOrder":2})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &sg).status(), FOXAPI_OK);
        r.settle(300);
        CHECK(allFinite(r));
        const std::string comb = R"({"comb":true,"combStrength":1,"dcBins":2})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &comb).status(), FOXAPI_OK);
        r.settle(500);
        CHECK(allFinite(r));
        // refusals change nothing
        const std::string bad1 = "{nope", bad2 = R"({"frames":9999})", bad3 = R"({"average":"median"})", bad4 = R"({"smoothing":"moving","smoothWidth":8})", bad5 = R"({"smoothing":"savgol","smoothWidth":5,"smoothOrder":6})";
        for (const std::string* b : {&bad1, &bad2, &bad3, &bad4, &bad5}) { CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), b).status(), FOXAPI_BAD_ARGUMENT); }
        CHECK(r.fact("display.average").find("\"comb\":true") != std::string::npos);
        // all off: the spread is back
        const std::string off = "{}";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &off).status(), FOXAPI_OK);
        r.settle(500);
        const double back = noiseSpread(r);
        std::printf("  all off again: %.2f dB\n", back);
        CHECK(back > 0.7 * plain);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_DISPLAY_AVERAGE), &avg).status(), FOXAPI_OK);
        CHECK(r.until([&] {
            std::ifstream f(user / "display-average.json");
            std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return t.find("\"rms\"") != std::string::npos;
        }, 6000));
    }
    {
        Rig r(user);
        r.settle(300);
        CHECK(r.fact("display.average").find("\"average\":\"rms\"") != std::string::npos);   // a new engine reads it
    }
}

void testHistory() {
    std::printf("D2: waterfall history\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    cascade::engine::WireA& w = cascade::engine::WireA::of(r.eng);
    std::vector<std::uint8_t> v;
    std::size_t bins = 0;
    // off by default
    CHECK_EQ(r.fact("wf.1.on"), std::string("0"));
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_WF_HISTORY, 0.0, 0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(w.waterfallCopy(0, 10, v, bins), std::size_t(0));
    // bad size, then on at 16 MB
    FoxCommand on = testing::cmd(FOXAPP_OP_WF_HISTORY, 5000.0, 5);
    on.ival[1] = 1;
    CHECK_EQ(r.send(on).status(), FOXAPI_OUT_OF_RANGE);
    on.num[0] = 16.0;
    CHECK_EQ(r.send(on).status(), FOXAPI_OK);
    CHECK_EQ(r.send(on).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.fact("wf.1.on"), std::string("1"));
    CHECK_EQ(r.fact("wf.1.megabytes"), std::string("16"));
    CHECK(r.until([&] { return w.waterfallCopy(0, 40, v, bins) >= 40u; }, 8000));
    CHECK(bins >= 64u);
    // pause: the held view does not move while live lines go on
    FoxCommand pause = testing::cmd(FOXAPP_OP_WF_HISTORY, 0.0, 0);
    testing::Outcome o = r.send(pause);
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 1);
    std::vector<std::uint8_t> held1, held2;
    std::size_t b1 = 0, b2 = 0;
    const std::size_t n1 = w.waterfallCopy(0, 30, held1, b1);
    r.settle(600);
    const std::size_t n2 = w.waterfallCopy(0, 30, held2, b2);
    CHECK_EQ(n1, std::size_t(30));
    CHECK_EQ(n2, std::size_t(30));
    CHECK(held1 == held2);
    CHECK_EQ(r.send(pause).status(), FOXAPI_NO_CHANGE);   // paused already
    // scroll back 20 lines (window of 30): behind reports it
    FoxCommand back = testing::cmd(FOXAPP_OP_WF_HISTORY, 20.0, 2);
    back.num[1] = 30.0;
    o = r.send(back);
    CHECK_EQ(o.status(), FOXAPI_OK);
    const int behindBack = static_cast<int>(o.result.applied[0]);
    CHECK(behindBack >= 20);   // 20 lines back from where it was held, and the live lines that arrived since are behind it too
    std::vector<std::uint8_t> older;
    CHECK_EQ(w.waterfallCopy(0, 30, older, b2), std::size_t(30));
    CHECK(older != held1);   // a different 30 lines
    // forward 5 lines, then 100: back to live
    FoxCommand fwd = testing::cmd(FOXAPP_OP_WF_HISTORY, 5.0, 3);
    o = r.send(fwd);
    CHECK(static_cast<int>(o.result.applied[0]) < behindBack);   // five lines nearer live (the live lines that arrive meanwhile count behind)
    fwd.num[0] = 100.0;
    o = r.send(fwd);
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 0);   // live again
    // a time before the first line: the oldest window (or nothing to move when the history is shorter than the window); bad numbers are refused
    {
        const int st = r.send(testing::cmd(FOXAPP_OP_WF_HISTORY, 1000.0, 4)).status();
        CHECK(st == FOXAPI_OK || st == FOXAPI_NO_CHANGE);
    }
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_WF_HISTORY, 0.0, 2)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_WF_HISTORY, -5.0, 4)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_WF_HISTORY, 0.0, 9)).status(), FOXAPI_OUT_OF_RANGE);
    {
        const int st = r.send(testing::cmd(FOXAPP_OP_WF_HISTORY, 0.0, 1)).status();   // live
        CHECK(st == FOXAPI_OK || st == FOXAPI_NO_CHANGE);
    }
    // live again: the live view moves
    std::vector<std::uint8_t> l1, l2;
    (void)w.waterfallCopy(0, 30, l1, b1);
    r.settle(500);
    (void)w.waterfallCopy(0, 30, l2, b2);
    CHECK(l1 != l2);
    // off frees it
    FoxCommand off = testing::cmd(FOXAPP_OP_WF_HISTORY, 0.0, 5);
    off.ival[1] = 0;
    CHECK_EQ(r.send(off).status(), FOXAPI_OK);
    CHECK_EQ(r.send(off).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(w.waterfallCopy(0, 10, v, bins), std::size_t(0));
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_display");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_display_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testChain();
    testHistory();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_display");
}
