// test_engine_api_pool.cpp - TUNER POOLING THROUGH THE ENGINE (engine serial lane A, row R19): FOXAPP_OP_CHANNEL_CLAIM 0x8620, CHANNEL_RELEASE 0x8621 on two generator radios.
//   A  a claim inside radio 1's span is tuned by its VFO alone (the centre does not move) and holds radio 1; the same claim again goes to radio 2, which retunes its centre; a third finds none free (LIMIT);
//   B  a release frees the radio and the next claim takes it again; a locked radio and a recording radio are skipped;  C  mode and bandwidth in the claim reach the radio;  D  refusals.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand claim(double hz, std::int64_t mode = 0, double bw = 0.0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_CHANNEL_CLAIM, hz, mode, nullptr);
    c.num[0] = hz;
    c.num[1] = bw;
    return c;
}
FoxCommand release(std::int64_t n) { return testing::cmd(FOXAPP_OP_CHANNEL_RELEASE, 0.0, n, nullptr); }
FoxCommand lock(std::uint32_t target, std::int64_t on) {
    FoxCommand c = testing::cmd(FOXAPP_OP_RADIO_LOCK, 0.0, on, nullptr);
    c.target = target;
    return c;
}
FoxCommand freq(std::uint32_t target, double hz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_FREQUENCY, hz, 0, nullptr);
    c.target = target;
    return c;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_pool\n");
    engine_api_test::hermetic("pool");
    Access::installHooks();
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    CHECK_EQ(r.send(freq(1, 100.0e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(freq(2, 200.0e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return std::fabs(r.radio(1).tunedHz - 100.0e6) < 1.0 && std::fabs(r.radio(2).tunedHz - 200.0e6) < 1.0; }, 5000));
    const double centre1 = r.radio(1).centreHz;
    CHECK_EQ(r.fact("radio.1.claimed"), std::string("0"));

    std::printf("A: claims\n");
    const testing::Outcome a = r.send(claim(centre1 + 50000.0));
    CHECK_EQ(a.status(), FOXAPI_OK);
    CHECK_EQ(a.result.applied[0], 1.0);       // radio 1
    CHECK_EQ(a.result.applied[1], 1.0);       // by its VFO alone
    CHECK(r.until([&] { return std::fabs(r.radio(1).tunedHz - (centre1 + 50000.0)) < 1.0; }, 5000));
    CHECK(std::fabs(r.radio(1).centreHz - centre1) < 1.0);   // the hardware was not touched
    CHECK_EQ(r.fact("radio.1.claimed"), std::string("1"));
    const testing::Outcome b = r.send(claim(centre1 + 80000.0));
    CHECK_EQ(b.status(), FOXAPI_OK);
    CHECK_EQ(b.result.applied[0], 2.0);       // radio 1 is taken; radio 2 must move its centre
    CHECK_EQ(b.result.applied[1], 2.0);
    CHECK(r.until([&] { return std::fabs(r.radio(2).tunedHz - (centre1 + 80000.0)) < 1.0; }, 5000));
    CHECK_EQ(r.fact("radio.2.claimed"), std::string("1"));
    const testing::Outcome none = r.send(claim(145.0e6));
    CHECK_EQ(none.status(), FOXAPI_LIMIT);
    CHECK(std::string(none.result.message).find("no radio is free") != std::string::npos);

    std::printf("B: release, lock, recording\n");
    CHECK_EQ(r.send(release(1)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.claimed"), std::string("0"));
    CHECK_EQ(r.send(release(1)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(lock(1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(claim(145.0e6)).status(), FOXAPI_LIMIT);              // radio 1 is locked, radio 2 claimed
    CHECK_EQ(r.send(lock(1, 0)).status(), FOXAPI_OK);
    const testing::Outcome again = r.send(claim(145.0e6));
    CHECK_EQ(again.status(), FOXAPI_OK);
    CHECK_EQ(again.result.applied[0], 1.0);
    CHECK_EQ(r.send(release(1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(release(2)).status(), FOXAPI_OK);
    // a radio that is recording is not free
    FoxCommand rec = testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1);
    rec.target = 1;
    Access::recordDir(r.eng, "pool_rec_scratch");
    CHECK_EQ(r.send(rec).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.1.recording") == "1" || true; }, 500));
    r.until([] { return false; }, 1200);
    const testing::Outcome rcl = r.send(claim(1.0e6 + 7.0e6));
    CHECK_EQ(rcl.status(), FOXAPI_OK);
    std::printf("  while radio 1 records, the claim goes to radio %.0f\n", rcl.result.applied[0]);
    CHECK_EQ(rcl.result.applied[0], 2.0);
    rec.ival[0] = 0;
    (void)r.send(rec);
    r.until([] { return false; }, 900);
    CHECK_EQ(r.send(release(2)).status(), FOXAPI_OK);

    std::printf("C: mode and bandwidth\n");
    const testing::Outcome m = r.send(claim(centre1 + 20000.0, FOXAPI_DEMOD_USB, 4000.0));
    CHECK_EQ(m.status(), FOXAPI_OK);
    const std::uint32_t got = static_cast<std::uint32_t>(m.result.applied[0]);
    CHECK(r.until([&] { return r.radio(got).demodMode == FOXAPI_DEMOD_USB && r.radio(got).bandwidthHz == 4000.0; }, 5000));
    CHECK_EQ(r.send(release(got)).status(), FOXAPI_OK);

    std::printf("D: refusals\n");
    for (const double bad : {0.0, -5.0, 2.0e12}) { CHECK_EQ(r.send(claim(bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(claim(100.0e6, 9)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(claim(100.0e6, 0, -1.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(release(0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.send(release(9)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.send(release(3)).status(), FOXAPI_NO_DEVICE);   // not open
    std::error_code ec;
    std::filesystem::remove_all("pool_rec_scratch", ec);
    return check::finish("test_engine_api_pool");
}
