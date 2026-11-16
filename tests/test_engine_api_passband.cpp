// test_engine_api_passband.cpp - THE PASSBAND'S EDGES AND THE AM DETECTOR THROUGH THE ENGINE (screen lane S1, wave 8; rows G1-35, G1-34, G1-19): FOXAPP_OP_SET_PASSBAND 0x8630 and FOXAPP_OP_SET_AM_DETECTOR 0x8631.
//   A  off at the start with the voice window, fact.radio.1.passband reads "0;300;2700;0;0";  B  the figures are taken, applied[] says them and the fact follows (the last field, whether the band-pass is really in
//   the chain, is the DSP thread's and is read by the drive script s1-signal-radio and tests/test_pipeline_passband.cpp: this rig runs no DSP);  C  in a mode with none (NFM) the figures are kept;  D  refusals change nothing (a cut or a shift out of range, the same figures again);  E  the AM detector: the
//   envelope at the start, each kind read back from fact.radio.1.amdet, a kind out of range refused;  F  each one acts on the radio it is aimed at (radio 9 is absent).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand passband(std::uint32_t target, std::int64_t on, double low, double high, double shift) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SET_PASSBAND, low, on, nullptr);
    c.ival[1] = 1;
    c.num[1] = high;
    c.num[2] = shift;
    c.target = target;
    return c;
}
FoxCommand passbandOnly(std::uint32_t target, std::int64_t on) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SET_PASSBAND, 0.0, on, nullptr);
    c.target = target;
    return c;
}
FoxCommand detector(std::uint32_t target, std::int64_t kind) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SET_AM_DETECTOR, 0.0, kind, nullptr);
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
    std::printf("test_engine_api_passband\n");
    engine_api_test::hermetic("passband");
    Access::installHooks();
    Rig r;
    r.pump(10);

    std::printf("A: the start\n");
    CHECK_EQ(r.fact("radio.1.passband"), std::string("0;300;2700;0;0"));
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_USB)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).demodMode == FOXAPI_DEMOD_USB; }, 3000));

    std::printf("B: the figures are taken\n");
    const testing::Outcome o = r.send(passband(1, 1, 500.0, 2000.0, 200.0));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(o.result.applied[0], 500.0);
    CHECK_EQ(o.result.applied[1], 2000.0);
    CHECK(r.until([&] { return r.fact("radio.1.passband").rfind("1;500;2000;200;", 0) == 0; }, 3000));
    CHECK_EQ(r.send(passbandOnly(1, 0)).status(), FOXAPI_OK);                    // off keeps the figures
    CHECK(r.until([&] { return r.fact("radio.1.passband") == "0;500;2000;200;0"; }, 3000));
    CHECK_EQ(r.send(passbandOnly(1, 1)).status(), FOXAPI_OK);                    // on again with them
    CHECK(r.until([&] { return r.fact("radio.1.passband").rfind("1;500;2000;200;", 0) == 0; }, 3000));

    std::printf("C: a mode with no passband\n");
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).demodMode == FOXAPI_DEMOD_NFM; }, 3000));
    CHECK(r.until([&] { return r.fact("radio.1.passband").rfind("1;500;2000;200;", 0) == 0; }, 3000));   // kept in a mode that has none
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_CW)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.1.passband").rfind("1;500;2000;200;", 0) == 0; }, 3000));

    std::printf("D: refusals\n");
    CHECK_EQ(r.send(passband(1, 1, 500.0, 2000.0, 200.0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(passband(1, 1, -1.0, 2000.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(passband(1, 1, 500.0, 24001.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(passband(1, 1, 500.0, 2000.0, 6001.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK(r.send(passband(1, 1, std::nan(""), 2000.0, 0.0)).status() != FOXAPI_OK);          // not a number: the command is refused before the engine applies it
    CHECK(r.fact("radio.1.passband").rfind("1;500;2000;200;", 0) == 0);                  // none of them changed it

    std::printf("E: the AM detector\n");
    CHECK_EQ(r.fact("radio.1.amdet").substr(0, 2), std::string("0;"));
    for (const std::int64_t k : {1, 2, 3, 0}) {
        CHECK_EQ(r.send(detector(1, k)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.1.amdet").rfind(std::to_string(k) + ";", 0) == 0; }, 3000));
    }
    CHECK_EQ(r.send(detector(1, 0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(detector(1, 4)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(detector(1, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.fact("radio.1.amdet").substr(0, 2), std::string("0;"));

    std::printf("F: an absent radio\n");
    CHECK_EQ(r.send(passband(9, 1, 300.0, 2700.0, 0.0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.send(detector(9, 1)).status(), FOXAPI_NO_DEVICE);
    return check::finish("test_engine_api_passband");
}
