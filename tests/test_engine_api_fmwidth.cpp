// test_engine_api_fmwidth.cpp - THE FM WIDTH PRESETS THROUGH THE ENGINE (engine serial lane A, row D7): FOXAPP_OP_SET_FM_WIDTH 0x861F.
//   A  in NFM and in WFM each preset sets the width it names (narrow 12.5, medium 30, medium-wide 50, broadcast 150 kHz) and super-wide is held at 0.9 of the channel rate with CLAMPED;
//   C  refusals: outside 0..4, in AM, an absent radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>

#include "wire1_rig.hpp"

namespace {

FoxCommand width(std::uint32_t target, std::int64_t preset) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SET_FM_WIDTH, 0.0, preset, nullptr);
    c.target = target;
    return c;
}
FoxCommand mode(std::uint32_t target, std::int64_t m) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_MODE, 0.0, m, nullptr);
    c.target = target;
    return c;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_fmwidth\n");
    engine_api_test::hermetic("fmwidth");
    Access::installHooks();
    Rig r;
    r.pump(10);
    const double hi = 0.9 * r.radio(1).channelRateHz;
    std::printf("A: the presets (channel rate %.0f Hz, so the top is %.0f Hz)\n", r.radio(1).channelRateHz, hi);
    const double want[4] = {12500.0, 30000.0, 50000.0, 150000.0};
    for (const std::int64_t m : {FOXAPI_DEMOD_NFM, FOXAPI_DEMOD_WFM}) {
        CHECK_EQ(r.send(mode(1, m)).status(), FOXAPI_OK);
        for (int i = 0; i < 4; ++i) {
            const testing::Outcome o = r.send(width(1, i));
            CHECK_EQ(o.status(), FOXAPI_OK);
            CHECK_EQ(o.result.applied[0], std::fmin(want[i], hi));
            CHECK(r.until([&] { return r.radio(1).bandwidthHz == std::fmin(want[i], hi); }, 3000));
        }
        const testing::Outcome sw = r.send(width(1, 4));
        CHECK_EQ(sw.status(), FOXAPI_OK);
        CHECK_EQ(sw.result.applied[0], std::fmin(225000.0, hi));
        CHECK_EQ((sw.result.flags & FOXAPI_RESULT_CLAMPED) != 0u, 225000.0 > hi);
    }

    std::printf("C: refusals\n");
    CHECK_EQ(r.send(width(1, 5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(width(1, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(width(9, 1)).status(), FOXAPI_NO_DEVICE);
    const double bwBefore = r.radio(1).bandwidthHz;
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).demodMode == FOXAPI_DEMOD_AM; }, 3000));
    const double amBw = r.radio(1).bandwidthHz;
    CHECK_EQ(r.send(width(1, 1)).status(), FOXAPI_UNSUPPORTED);
    CHECK_EQ(r.radio(1).bandwidthHz, amBw);
    (void)bwBefore;
    return check::finish("test_engine_api_fmwidth");
}
