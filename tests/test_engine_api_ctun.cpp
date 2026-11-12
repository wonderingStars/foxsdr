// test_engine_api_ctun.cpp - CENTRE TUNING THROUGH THE ENGINE (engine serial lane A, row T4): SET_CTUN 0x0122, per radio, default off.
//   A  off (the old rule): a tune keeps the VFO offset and the centre follows;  B  on: a tune inside the span on show moves the VFO and leaves the centre, one outside it moves the centre and
//   keeps the offset, the tuned frequency is the one asked for either way;  C  the switch: NO_CHANGE when it is so already, per radio, refusals;  D  saved per radio and restored.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand ctun(std::uint32_t target, std::int64_t on) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_CTUN, 0.0, on, nullptr);
    c.target = target;
    return c;
}
FoxCommand freq(std::uint32_t target, double hz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_FREQUENCY, hz, 0, nullptr);
    c.target = target;
    return c;
}
bool at(Rig& r, std::uint32_t radio, double tuned) { return std::fabs(r.radio(radio).tunedHz - tuned) < 1.0; }

void testAB() {
    std::printf("A, B: the two rules\n");
    Rig r;
    r.pump(10);
    CHECK_EQ(r.radio(1).ctun, 0u);
    CHECK_EQ(r.send(freq(1, 100.0e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return at(r, 1, 100.0e6); }, 5000));
    const double rate = r.radio(1).sampleRateHz;
    CHECK(rate >= 500000.0);
    // A: off. Put an offset on first (a tune that lands off the centre only happens with one), then tune 20 kHz up: the offset is kept, the centre follows
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 30000.0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return std::fabs(r.radio(1).vfoOffsetHz - 30000.0) < 1.0; }, 5000));
    const double centre0 = r.radio(1).centreHz;
    CHECK_EQ(r.send(freq(1, 100.0e6 + 20000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return at(r, 1, 100.0e6 + 20000.0); }, 5000));
    CHECK(std::fabs(r.radio(1).vfoOffsetHz - 30000.0) < 1.0);
    CHECK(std::fabs(r.radio(1).centreHz - (100.0e6 + 20000.0 - 30000.0)) < 1.0);   // the centre is what puts the kept offset under the asked frequency
    (void)centre0;

    // B: on, inside the span: the centre stays, the offset moves
    CHECK_EQ(r.send(ctun(1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.radio(1).ctun, 1u);
    const double centre1 = r.radio(1).centreHz;
    CHECK_EQ(r.send(freq(1, centre1 + 60000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return at(r, 1, centre1 + 60000.0); }, 5000));
    CHECK(std::fabs(r.radio(1).centreHz - centre1) < 1.0);
    CHECK(std::fabs(r.radio(1).vfoOffsetHz - 60000.0) < 1.0);
    // and back below the centre
    CHECK_EQ(r.send(freq(1, centre1 - 100000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return at(r, 1, centre1 - 100000.0); }, 5000));
    CHECK(std::fabs(r.radio(1).centreHz - centre1) < 1.0);
    CHECK(std::fabs(r.radio(1).vfoOffsetHz + 100000.0) < 1.0);
    // outside the span: the centre moves and the offset is kept, the tuned frequency is still the one asked for
    const double farHz = centre1 + 5.0e6;
    const double offBefore = r.radio(1).vfoOffsetHz;
    CHECK_EQ(r.send(freq(1, farHz)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return at(r, 1, farHz); }, 5000));
    CHECK(std::fabs(r.radio(1).vfoOffsetHz - offBefore) < 1.0);
    CHECK(std::fabs(r.radio(1).centreHz - (farHz - offBefore)) < 1.0);
    // the edge of the span: a tune whose channel would hang off the usable 90 % of the band moves the centre
    const double centre2 = r.radio(1).centreHz;
    const double edge = centre2 + 0.44 * rate;   // inside the 0.45 limit as a point, outside it with half the channel added
    CHECK_EQ(r.send(freq(1, edge)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return at(r, 1, edge); }, 5000));
    CHECK(std::fabs(r.radio(1).centreHz - centre2) > 1.0);   // the centre moved

    // C: the switch
    CHECK_EQ(r.send(ctun(1, 1)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(ctun(1, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(ctun(1, 0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(ctun(9, 1)).status(), FOXAPI_NO_DEVICE);
    const double c3 = r.radio(1).centreHz, o3 = r.radio(1).vfoOffsetHz;
    CHECK_EQ(r.send(freq(1, c3 + o3 + 10000.0)).status(), FOXAPI_OK);   // off again: the old rule
    CHECK(r.until([&] { return std::fabs(r.radio(1).centreHz - (c3 + 10000.0)) < 1.0; }, 5000));
    CHECK(std::fabs(r.radio(1).vfoOffsetHz - o3) < 1.0);
}

void testD() {
    std::printf("D: per radio, saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(ctun(2, 1)).status(), FOXAPI_OK);
        a.pump(10);
        CHECK_EQ(a.radio(1).ctun, 0u);
        CHECK_EQ(a.radio(2).ctun, 1u);
        file = throughTheFile(a.eng);
        CHECK(!file.ctun);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK(file.radios[0].ctun);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).ctun, 0u);
        CHECK_EQ(b.radio(2).ctun, 1u);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_ctun\n");
    engine_api_test::hermetic("ctun");
    Access::installHooks();
    testAB();
    testD();
    return check::finish("test_engine_api_ctun");
}
