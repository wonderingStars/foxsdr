// test_engine_api_vfoab.cpp - VFO A / B THROUGH THE ENGINE (engine serial lane A, row T6): FOXAPP_OP_VFO_SELECT 0x861C, VFO_SWAP 0x861D, VFO_STORE 0x861E, per radio, receive only.
//   A  the first B is the radio's own tune; tuning B and coming back finds A where it was (frequency, mode and bandwidth);  B  a swap exchanges the two and swapping twice restores them;
//   C  STORE copies the radio's tune into the other letter or sets it explicitly;  D  refusals: no such letter, the letter already in use, a swap with nothing in the other VFO, a locked
//   frequency, a register that is not a register, an absent radio;  E  saved per radio and restored in an in-process restart, junk in a file lands on "never set".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand sel(std::uint32_t target, std::int64_t letter) {
    FoxCommand c = testing::cmd(FOXAPP_OP_VFO_SELECT, 0.0, letter, nullptr);
    c.target = target;
    return c;
}
FoxCommand swp(std::uint32_t target) {
    FoxCommand c = testing::cmd(FOXAPP_OP_VFO_SWAP, 0.0, 0, nullptr);
    c.target = target;
    return c;
}
FoxCommand store(std::uint32_t target, std::int64_t explicitSet, std::int64_t mode, double hz, double bw, double letter) {
    FoxCommand c = testing::cmd(FOXAPP_OP_VFO_STORE, hz, explicitSet, nullptr);
    c.target = target;
    c.ival[1] = mode;
    c.num[0] = hz;
    c.num[1] = bw;
    c.num[2] = letter;
    return c;
}
FoxCommand tune(std::uint32_t target, double hz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_FREQUENCY, hz, 0, nullptr);
    c.target = target;
    return c;
}
FoxCommand mode(std::uint32_t target, std::int64_t m) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_MODE, 0.0, m, nullptr);
    c.target = target;
    return c;
}
FoxCommand bw(std::uint32_t target, double hz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_BANDWIDTH, hz, 0, nullptr);
    c.target = target;
    return c;
}
FoxCommand lock(std::uint32_t target, std::int64_t on) {
    FoxCommand c = testing::cmd(FOXAPP_OP_RADIO_LOCK, 0.0, on, nullptr);
    c.target = target;
    return c;
}

bool tunedAt(Rig& r, std::uint32_t radio, double hz) { return std::fabs(r.radio(radio).tunedHz - hz) < 1.0; }

void testAB() {
    std::printf("A, B, C: select, swap, store\n");
    Rig r;
    r.pump(10);
    CHECK_EQ(r.send(tune(1, 7100000.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_LSB)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(bw(1, 4000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return tunedAt(r, 1, 7100000.0) && r.radio(1).demodMode == FOXAPI_DEMOD_LSB && r.radio(1).bandwidthHz == 4000.0; }, 5000));
    CHECK_EQ(r.radio(1).vfoActive, 0u);
    CHECK_EQ(r.radio(1).vfoOtherMode, 0u);   // never set

    // A: the first B is the radio's own tune
    CHECK_EQ(r.send(sel(1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.radio(1).vfoActive, 1u);
    CHECK(tunedAt(r, 1, 7100000.0) && r.radio(1).demodMode == FOXAPI_DEMOD_LSB);
    CHECK_EQ(r.radio(1).vfoOtherHz, 7100000.0);              // A, remembered in the register
    CHECK_EQ(r.radio(1).vfoOtherMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_LSB));
    // tune B somewhere else, in another mode and width
    CHECK_EQ(r.send(tune(1, 14200000.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_USB)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(bw(1, 5000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return tunedAt(r, 1, 14200000.0) && r.radio(1).demodMode == FOXAPI_DEMOD_USB; }, 5000));
    // back to A: where it was
    CHECK_EQ(r.send(sel(1, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return tunedAt(r, 1, 7100000.0) && r.radio(1).demodMode == FOXAPI_DEMOD_LSB && r.radio(1).bandwidthHz == 4000.0; }, 5000));
    CHECK_EQ(r.radio(1).vfoActive, 0u);
    CHECK_EQ(r.radio(1).vfoOtherHz, 14200000.0);
    CHECK_EQ(r.radio(1).vfoOtherMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_USB));
    CHECK_EQ(r.radio(1).vfoOtherBandwidthHz, 5000.0);

    // B: swap, and swap back
    CHECK_EQ(r.send(swp(1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return tunedAt(r, 1, 14200000.0) && r.radio(1).demodMode == FOXAPI_DEMOD_USB && r.radio(1).bandwidthHz == 5000.0; }, 5000));
    CHECK_EQ(r.radio(1).vfoActive, 0u);                      // the letter stays; the contents trade places
    CHECK_EQ(r.radio(1).vfoOtherHz, 7100000.0);
    CHECK_EQ(r.send(swp(1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return tunedAt(r, 1, 7100000.0) && r.radio(1).demodMode == FOXAPI_DEMOD_LSB && r.radio(1).bandwidthHz == 4000.0; }, 5000));
    CHECK_EQ(r.radio(1).vfoOtherHz, 14200000.0);

    // C: store copies, or sets explicitly
    CHECK_EQ(r.send(store(1, 0, 0, 0.0, 0.0, 0.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.radio(1).vfoOtherHz, 7100000.0);
    CHECK_EQ(r.radio(1).vfoOtherMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_LSB));
    CHECK_EQ(r.send(store(1, 1, FOXAPI_DEMOD_AM, 1008000.0, 9000.0, 0.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.radio(1).vfoOtherHz, 1008000.0);
    CHECK_EQ(r.radio(1).vfoOtherMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_AM));
    CHECK_EQ(r.radio(1).vfoOtherBandwidthHz, 9000.0);
    CHECK(tunedAt(r, 1, 7100000.0));                         // storing does not retune

    // D: refusals
    CHECK_EQ(r.send(sel(1, 2)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(sel(1, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(sel(1, 0)).status(), FOXAPI_NO_CHANGE);  // A is in use
    CHECK_EQ(r.send(store(1, 2, 0, 0.0, 0.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(store(1, 1, 0, 1.0e6, 3000.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);        // mode 0
    CHECK_EQ(r.send(store(1, 1, 9, 1.0e6, 3000.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);        // mode 9
    CHECK_EQ(r.send(store(1, 1, FOXAPI_DEMOD_AM, -5.0, 3000.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(store(1, 1, FOXAPI_DEMOD_AM, 1.0e6, -1.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(store(1, 1, FOXAPI_DEMOD_AM, 1.0e6, 3000.0, 2.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).vfoOtherHz, 1008000.0);              // none of them changed it
    CHECK_EQ(r.send(lock(1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(sel(1, 1)).status(), FOXAPI_DENIED);
    CHECK_EQ(r.send(swp(1)).status(), FOXAPI_DENIED);
    CHECK(tunedAt(r, 1, 7100000.0));
    CHECK_EQ(r.send(lock(1, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(sel(9, 1)).status(), FOXAPI_NO_DEVICE);
}

void testSwapNeedsASet() {
    std::printf("D: a swap with nothing in the other VFO\n");
    Rig r;
    r.pump(10);
    CHECK_EQ(r.send(swp(1)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.radio(1).vfoOtherMode, 0u);
}

void testE() {
    std::printf("E: saved and restored\n");
    AppConfig file;
    double aHz = 0.0;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(store(1, 1, FOXAPI_DEMOD_USB, 14070000.0, 5000.0, 1.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(store(2, 1, FOXAPI_DEMOD_AM, 9500000.0, 6000.0, 0.0)).status(), FOXAPI_OK);
        a.pump(20);
        aHz = a.radio(1).tunedHz;
        file = throughTheFile(a.eng);
        CHECK_EQ(file.vfoActive, 1);
        CHECK_EQ(file.vfoOtherHz, 14070000.0);
        CHECK_EQ(file.vfoOtherMode, static_cast<int>(FOXAPI_DEMOD_USB));
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].vfoOtherHz, 9500000.0);
        CHECK_EQ(file.radios[0].vfoActive, 0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).vfoActive, 1u);
        CHECK_EQ(b.radio(1).vfoOtherHz, 14070000.0);
        CHECK_EQ(b.radio(1).vfoOtherBandwidthHz, 5000.0);
        CHECK_EQ(b.radio(2).vfoOtherMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_AM));
        CHECK_EQ(b.radio(2).vfoActive, 0u);
        (void)aHz;
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"vfoActive\":7,\"vfoOtherHz\":-5,\"vfoOtherMode\":99,\"vfoOtherBwHz\":-1}", "test", back, error));
    CHECK_EQ(back.vfoActive, 0);
    CHECK_EQ(back.vfoOtherMode, 0);
    CHECK_EQ(back.vfoOtherHz, 0.0);
    CHECK_EQ(back.vfoOtherBwHz, 0.0);
    CHECK(ConfigStore::parse("{\"vfoActive\":1,\"vfoOtherHz\":7100000,\"vfoOtherMode\":5,\"vfoOtherBwHz\":4000}", "test", back, error));
    CHECK_EQ(back.vfoActive, 1);
    CHECK_EQ(back.vfoOtherHz, 7100000.0);
    CHECK_EQ(back.vfoOtherMode, 5);
    // a frequency without a mode is not a register
    CHECK(ConfigStore::parse("{\"vfoOtherHz\":7100000,\"vfoOtherMode\":0}", "test", back, error));
    CHECK_EQ(back.vfoOtherHz, 0.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_vfoab\n");
    engine_api_test::hermetic("vfoab");
    Access::installHooks();
    testAB();
    testSwapNeedsASet();
    testE();
    return check::finish("test_engine_api_vfoab");
}
