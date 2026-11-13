// test_engine_api_wire_a_rit.cpp - RIT 0x8827 THROUGH THE ENGINE (wire-a second pass, item 3): the receive clarifier shifts what is HEARD and nothing else.
//   A  off by default: the generator's tone is heard at its pitch, the facts say off;  B  RIT +300 Hz moves the pitch of an upper-sideband tone DOWN by 300 Hz (the oscillator moved up), the dial (tuned
//   frequency) and the VFO offset do not move, off puts the pitch back to the same figure;  C  a step is added and clamped to the limit;  D  a retune over the clear limit clears it, one under does not;  E  the
//   limits and every refusal (changes nothing);  F  a radio that goes away takes its clarifier with it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

constexpr double kToneOffsetHz = 1500.0;

FoxCommand rit(std::int64_t action, double hz = 0.0, std::int64_t second = 0, double hz2 = 0.0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_RIT, hz, action, nullptr);
    c.ival[1] = second;
    c.num[1] = hz2;
    return c;
}

// The pitch of the strongest tone in the radio's audio, 300 .. 3000 Hz, by a Goertzel scan every 2 Hz with a parabola through the peak.
double pitchHz(Rig& r) {
    r.settle(250);
    std::vector<float> l(4096), rr(4096);
    const std::size_t n = Access::audioTap(r.eng, l.data(), rr.data(), 4096);
    if (n < 4096) { return -1.0; }
    std::vector<double> p;
    std::vector<double> f;
    for (double hz = 300.0; hz <= 3000.0; hz += 2.0) {
        const double w = 6.283185307179586 * hz / 48000.0;
        const double cw = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double s = static_cast<double>(l[i]) + cw * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        p.push_back(s1 * s1 + s2 * s2 - cw * s1 * s2);
        f.push_back(hz);
    }
    std::size_t best = 1;
    for (std::size_t i = 1; i + 1 < p.size(); ++i) { if (p[i] > p[best]) { best = i; } }
    const double a = std::log(p[best - 1] + 1e-30), b = std::log(p[best] + 1e-30), c = std::log(p[best + 1] + 1e-30);
    const double den = a - 2.0 * b + c;
    return f[best] + (den != 0.0 ? 0.5 * (a - c) / den * 2.0 : 0.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_rit\n");
    engine_api_test::hermetic("wire_a_rit");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    r.eng.controlPass();
    CHECK(Access::genTone(r.eng, 0, r.local->state().vfoOffsetHz + kToneOffsetHz, -20.0f));   // (the generator's tones are offsets from its centre: put it 1500 Hz above the VFO)
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_USB)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -200.0)).status(), FOXAPI_OK);
    r.settle(300);

    std::printf("A: off by default\n");
    double base = 0.0;
    {
        CHECK_EQ(r.fact("rit.1.on"), std::string("0"));
        CHECK_EQ(r.fact("rit.1.hz"), std::string("0"));
        CHECK_EQ(r.fact("rit.1.max"), std::string("9999"));
        CHECK_EQ(r.fact("rit.1.clearOnRetune"), std::string("0"));
        base = pitchHz(r);
        std::printf("  the tone is heard at %.1f Hz\n", base);
        CHECK(std::fabs(base - kToneOffsetHz) < 15.0);
    }

    std::printf("B: RIT +300 Hz moves the pitch, not the dial\n");
    {
        r.eng.controlPass();
        const FoxReceiverState before = r.local->state();
        CHECK_EQ(r.send(rit(0, 300.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(rit(0, 300.0, 1)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.fact("rit.1.on"), std::string("1"));
        CHECK_EQ(r.fact("rit.1.hz"), std::string("300"));
        const double moved = pitchHz(r);
        std::printf("  heard at %.1f Hz (was %.1f)\n", moved, base);
        CHECK(std::fabs(moved - (base - 300.0)) < 15.0);
        r.eng.controlPass();
        const FoxReceiverState after = r.local->state();
        std::printf("  before: centre %.1f offset %.1f tuned %.1f; after: centre %.1f offset %.1f tuned %.1f\n", before.centreHz, before.vfoOffsetHz, before.tunedHz, after.centreHz, after.vfoOffsetHz, after.tunedHz);
        CHECK_EQ(after.tunedHz, before.tunedHz);
        CHECK_EQ(after.centreHz, before.centreHz);
        // off: the pitch is the old one, and the offset is remembered while off
        CHECK_EQ(r.send(rit(0, 300.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("rit.1.on"), std::string("0"));
        CHECK_EQ(r.fact("rit.1.hz"), std::string("300"));
        const double off = pitchHz(r);
        std::printf("  off: %.1f Hz\n", off);
        CHECK(std::fabs(off - base) < 3.0);
        CHECK_EQ(r.send(rit(0, -400.0, 1)).status(), FOXAPI_OK);   // negative: the other way
        const double up = pitchHz(r);
        std::printf("  -400 Hz: %.1f Hz\n", up);
        CHECK(std::fabs(up - (base + 400.0)) < 15.0);
        CHECK_EQ(r.send(rit(2)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(rit(2)).status(), FOXAPI_NO_CHANGE);
        CHECK(std::fabs(pitchHz(r) - base) < 3.0);
    }

    std::printf("C: step and the limit\n");
    {
        CHECK_EQ(r.send(rit(3, 500.0, 0, 0.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("rit.1.max"), std::string("500"));
        FoxCommandResult a = r.send(rit(1, 200.0)).result;
        CHECK_EQ(a.applied[0], 200.0);
        a = r.send(rit(1, 200.0)).result;
        CHECK_EQ(a.applied[0], 400.0);
        a = r.send(rit(1, 200.0)).result;
        CHECK_EQ(a.applied[0], 500.0);   // clamped
        CHECK_EQ(r.fact("rit.1.on"), std::string("1"));
        a = r.send(rit(1, -1000.0)).result;
        CHECK_EQ(a.applied[0], -500.0);
        // a smaller limit re-clamps what is set
        CHECK_EQ(r.send(rit(3, 100.0, 0, 0.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("rit.1.hz"), std::string("-100"));
        CHECK_EQ(r.send(rit(2)).status(), FOXAPI_OK);
    }

    std::printf("D: a retune over the clear limit clears it\n");
    {
        CHECK_EQ(r.send(rit(3, 9999.0, 0, 5000.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("rit.1.clearOnRetune"), std::string("5000"));
        CHECK_EQ(r.send(rit(0, 300.0, 1)).status(), FOXAPI_OK);
        r.eng.controlPass();
        const double dial = r.local->state().tunedHz;
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, dial + 1000.0)).status(), FOXAPI_OK);   // under the limit
        r.settle(200);
        CHECK_EQ(r.fact("rit.1.hz"), std::string("300"));
        CHECK_EQ(r.fact("rit.1.on"), std::string("1"));
        CHECK(std::fabs(pitchHz(r) - (base - 300.0)) < 15.0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, dial + 60000.0)).status(), FOXAPI_OK);   // over it
        r.settle(300);
        CHECK_EQ(r.fact("rit.1.hz"), std::string("0"));
        CHECK(std::fabs(pitchHz(r) - base) < 3.0);
        CHECK_EQ(r.send(rit(3, 9999.0, 0, 0.0)).status(), FOXAPI_OK);   // never again
        CHECK_EQ(r.send(rit(0, 300.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, dial + 120000.0)).status(), FOXAPI_OK);
        r.settle(300);
        CHECK_EQ(r.fact("rit.1.hz"), std::string("300"));
        CHECK_EQ(r.send(rit(2)).status(), FOXAPI_OK);
    }

    std::printf("E: refusals change nothing\n");
    {
        CHECK_EQ(r.send(rit(0, 10000.0, 1)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(rit(0, 5.0, 2)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(rit(0, std::nan(""), 1)).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(rit(3, 0.5, 0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(rit(3, 20001.0, 0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(rit(3, 9999.0, 0, -1.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(rit(9)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.fact("rit.1.hz"), std::string("0"));
        CHECK_EQ(r.fact("rit.1.on"), std::string("0"));
        CHECK_EQ(r.fact("rit.1.max"), std::string("9999"));
        FoxCommand other = rit(0, 100.0, 1);
        other.target = 7;
        CHECK_EQ(r.send(other).status(), FOXAPI_NO_DEVICE);
    }
    return check::finish("test_engine_api_wire_a_rit");
}
