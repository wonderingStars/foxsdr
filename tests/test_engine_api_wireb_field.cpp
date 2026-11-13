// test_engine_api_wireb_field.cpp - field strength in the ENGINE (FIELD_TABLE 0x8900, FIELD_CONFIG 0x8901, FIELD_READ 0x8902; wire-b; model core/field_strength.hpp), held
// against the real engine through the C table, the radio on its signal generator.
//
//   F1  no table: FIELD_READ is FAILED with a sentence; nothing is invented
//   F2  an antenna-factor table (MHz) and the radio's tuned frequency: the answer is dBm -> dBuV + the table's factor, equal to the model's own figure for the
//       same level; the fact radio.1.field.readout carries "<x> dBuV/m at <f> MHz"
//   F3  a frequency outside the table: the end value is held and the held bit is set (and the fact says so)
//   F4  a cable table and a preamp: the cable loss adds, the preamp subtracts
//   F5  refusals: a bad table is BAD_ARGUMENT with its line and the OLD table stays; bad table / unit / preamp / impedance are OUT_OF_RANGE; clearing twice is NO_CHANGE
//   F6  a REMOTE session cannot send any of the three
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>

#include "core/field_strength.hpp"
#include "wire_b_rig.hpp"

using wireb::Rig;

namespace {

constexpr long long kMHz = 2;

FoxCommand tableCmd(long long which, long long unit) {
    FoxCommand c = testing::cmd(FOXAPP_OP_FIELD_TABLE, 0.0, which);
    c.ival[1] = unit;
    return c;
}

void startRadio(Rig& r, double hz) {
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, hz)).status(), FOXAPI_OK);
    r.passes(20);
}

void testNoTable() {
    std::printf("F1: no table\n");
    Rig r;
    startRadio(r, 100.1e6);
    const auto o = r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1));
    CHECK_EQ(o.status(), FOXAPI_FAILED);
    CHECK(std::string(o.result.message).find("antenna-factor") != std::string::npos);
    CHECK_EQ(r.fact("radio.1.field.readout"), std::string("<absent>"));
}

// What the readout must be: the model's own figure for the SAME level the readout reported (the readout fact carries the dBuV/m; the level is the radio's own
// signalDbm, which moves a little between two reads of a live generator, so the comparison is made against the generator's level read at the same time).
void testReadout() {
    std::printf("F2/F3/F4: the readout, the held end value, cable and preamp\n");
    Rig r;
    startRadio(r, 100.1e6);
    // AF 10 dB at 50 MHz, 30 dB at 150 MHz: linear in the value against log f
    CHECK_EQ(r.send(tableCmd(0, kMHz), "50,10\n150,30\n").status(), FOXAPI_OK);
    CHECK_EQ(r.fact("field.afPoints"), std::string("2"));
    const auto o = r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1));
    CHECK_EQ(o.status(), FOXAPI_OK);
    cascade::core::LossTable af;
    const std::string tbl = "50,10\n150,30\n";
    CHECK(af.parse(tbl.data(), tbl.size(), cascade::core::FreqUnit::MHz).ok());
    // The model's answer for AF alone at this frequency: the engine's reading minus the level's own dBuV is the factor
    const cascade::core::LookupResult f = af.lookup(100.1e6);
    CHECK(f.inRange());
    const double expectAf = 10.0 + 20.0 * std::log10(100.1 / 50.0) / std::log10(3.0);
    CHECK(std::fabs(f.valueDb - expectAf) < 1e-6);
    CHECK_EQ(static_cast<int>(o.result.applied[1]) & 3, 0);
    // the engine's figure equals the model's for the level the radio reports (read straight after: the generator's level is steady)
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    CHECK(wireb::api()->read_state(r.local->raw(), &st) >= 0);
    const cascade::core::LossTable noCable;
    const cascade::core::FieldReadout want = cascade::core::fieldStrengthFromTables(st.signalDbm, st.tunedHz, af, noCable);
    CHECK(want.valid);
    CHECK(std::fabs(o.result.applied[0] - want.dbuvPerM) < 1.0);
    const std::string line = r.fact("radio.1.field.readout");
    CHECK(line.find("dBuV/m at 100.100000 MHz") != std::string::npos);
    // F3: tune below the table's first point: the end value is held and says so
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 10.0e6)).status(), FOXAPI_OK);
    r.passes(20);
    const auto held = r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1));
    CHECK_EQ(held.status(), FOXAPI_OK);
    CHECK_EQ(static_cast<int>(held.result.applied[1]) & 1, 1);
    CHECK(r.fact("radio.1.field.readout").find("end value is held") != std::string::npos);
    // F4: a cable table of 3 dB everywhere adds 3 dB, a preamp of 20 dB takes 20 off (the level of the generator is steady to well under 1 dB)
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 100.1e6)).status(), FOXAPI_OK);
    r.passes(20);
    const double base = r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1)).result.applied[0];
    CHECK_EQ(r.send(tableCmd(1, kMHz), "1,3\n1000,3\n").status(), FOXAPI_OK);
    const double withCable = r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1)).result.applied[0];
    CHECK(std::fabs((withCable - base) - 3.0) < 1.0);
    FoxCommand cfg = testing::cmd(FOXAPP_OP_FIELD_CONFIG, 20.0, 1);
    cfg.num[1] = 50.0;
    CHECK_EQ(r.send(cfg).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("field.show"), std::string("1"));
    const double withPreamp = r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1)).result.applied[0];
    CHECK(std::fabs((withCable - withPreamp) - 20.0) < 1.0);
}

void testRefusals() {
    std::printf("F5: refusals\n");
    Rig r;
    CHECK_EQ(r.send(tableCmd(0, kMHz), "50,10\n150,30\n").status(), FOXAPI_OK);
    // frequencies out of order: refused with the model's words and the line; the old table stays
    const auto bad = r.send(tableCmd(0, kMHz), "50,10\n150,30\n100,20\n");
    CHECK_EQ(bad.status(), FOXAPI_BAD_ARGUMENT);
    CHECK(std::string(bad.result.message).find("must increase") != std::string::npos);
    CHECK(std::string(bad.result.message).find("line 3") != std::string::npos);
    CHECK_EQ(r.fact("field.afPoints"), std::string("2"));
    CHECK_EQ(r.send(tableCmd(2, kMHz), "1,1\n").status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(tableCmd(0, 4), "1,1\n").status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(tableCmd(0, kMHz), "1,abc\n").status(), FOXAPI_BAD_ARGUMENT);
    FoxCommand cfg = testing::cmd(FOXAPP_OP_FIELD_CONFIG, 101.0, 0);
    cfg.num[1] = 50.0;
    CHECK_EQ(r.send(cfg).status(), FOXAPI_OUT_OF_RANGE);
    cfg.num[0] = 0.0;
    cfg.num[1] = 0.5;
    CHECK_EQ(r.send(cfg).status(), FOXAPI_OUT_OF_RANGE);
    cfg.num[1] = 50.0;
    CHECK_EQ(r.send(cfg).status(), FOXAPI_NO_CHANGE);   // 0 dB, 50 ohm, hidden: what they were
    // clearing: once changes it, twice does not
    CHECK_EQ(r.send(tableCmd(0, kMHz), "").status(), FOXAPI_OK);
    CHECK_EQ(r.fact("field.afPoints"), std::string("0"));
    CHECK_EQ(r.send(tableCmd(0, kMHz), "").status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_FIELD_READ, 7)).status(), FOXAPI_NO_DEVICE);
}

void testRemote() {
    std::printf("F6: a REMOTE session cannot send them\n");
    Rig r;
    testing::Session remote(wireb::api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(r.send(tableCmd(0, kMHz), "50,10\n", &remote).status(), FOXAPI_DENIED);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FIELD_CONFIG, 0.0, 1), "", &remote).status(), FOXAPI_DENIED);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_FIELD_READ, 1), "", &remote).status(), FOXAPI_DENIED);
    CHECK_EQ(r.fact("field.afPoints"), std::string("<absent>"));   // nothing reached the engine: no state was even made
}

}  // namespace

int main() {
    engine_api_test::hermetic("wirebfield");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testNoTable();
    testReadout();
    testRefusals();
    testRemote();
    return check::finish("test_engine_api_wireb_field");
}
