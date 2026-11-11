// test_engine_api_tone.cpp - THE TONE SQUELCH THROUGH THE ENGINE (engine wiring lane 1, group B; docs/multi-radio-records/engine-wiring-1.md).
//
// The real engine through the C table and its own passes: ops 0x0113 SET_TONE_SQUELCH and 0x0114 SET_TONE_REMOVE.
//
//   A  defaults: both radios Off, any, any, any, remover off - the audio as it always was;
//   B  EACH RADIO HOLDS ITS OWN: radio 2 on CTCSS, radio 1 on DCS, set on one and read on both; the target rule (0 and 1 are radio 1, 2 is radio 2, 9 is NO_DEVICE);
//   C  the settings reach the radio's pipeline (its mirrors), and the measurements come back in the state: on a generator radio in NFM with a CTCSS tone required the carrier is
//      open and the tone is not there, so the tone gate is shut and nothing is detected; Off opens it again;
//   D  every refusal changes nothing: a kind past DCS, a tone that is not one of the 50, a code that is not one of the 83 (and one with a digit past 7), a polarity past
//      inverted - at the API's range check and again in the engine; the values not carried are the radio's last choice;
//   E  THE SETTINGS ARE SAVED: radio 1 in the plain keys, radio 2 in its `radios` entry; an in-process restart puts both back, in the engine and in its pipelines;
//   F  the web page, CAT and TCI profiles do not carry the two ops (the page has no control; a public REMOTE session with the SETTINGS grant may send them, as it may the
//      other audio settings).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "wire1_rig.hpp"

using cascade::engine::InternalKind;

namespace {

FoxCommand toneOp(std::uint32_t target, int kind, bool withValues, double hz, int code, int pol) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_TONE_SQUELCH, hz, kind, nullptr);
    c.ival[1] = withValues ? 1 : 0;
    c.num[1] = code;
    c.num[2] = pol;
    c.target = target;
    return c;
}

void testADefaults() {
    std::printf("A: defaults\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    for (std::uint32_t n : {1u, 2u}) {
        const FoxReceiverState s = r.radio(n);
        CHECK_EQ(s.toneKind, 0u);
        CHECK_EQ(s.toneHz, 0.0);
        CHECK_EQ(s.toneCode, 0u);
        CHECK_EQ(s.tonePolarity, 0u);
        CHECK_EQ(s.toneRemove, 0u);
        CHECK_EQ(s.toneDetectedHz, 0.0);
        CHECK_EQ(s.toneDetectedCode, 0u);
        CHECK_EQ(s.toneDetectedPolarity, 0u);
        CHECK_EQ(s.toneGate, 1u);   // no tone is required: the condition is met
    }
    CHECK_EQ(Access::pipeline(r.eng, 0).toneKind(), 0);
    CHECK_EQ(Access::pipeline(r.eng, 1).toneKind(), 0);
}

void testBEachRadioOwn() {
    std::printf("B: each radio holds its own\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    // radio 2: CTCSS 123.0
    CHECK_EQ(r.send(toneOp(2, FOXAPI_TONE_CTCSS, true, 123.0, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).toneKind == FOXAPI_TONE_CTCSS; }, 5000));
    CHECK_EQ(r.radio(2).toneHz, 123.0);
    CHECK_EQ(r.radio(1).toneKind, 0u);
    CHECK_EQ(r.radio(1).toneHz, 0.0);
    // radio 1 (target 1, then 0 = the focus): DCS 754 inverted-any
    CHECK_EQ(r.send(toneOp(1, FOXAPI_TONE_DCS, true, 0.0, 754, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).toneKind == FOXAPI_TONE_DCS; }, 5000));
    CHECK_EQ(r.radio(1).toneCode, 754u);
    CHECK_EQ(r.radio(1).tonePolarity, 1u);
    CHECK_EQ(r.radio(2).toneKind, static_cast<std::uint32_t>(FOXAPI_TONE_CTCSS));   // untouched by the other
    CHECK_EQ(r.radio(2).toneHz, 123.0);
    CHECK_EQ(r.radio(2).toneCode, 0u);
    CHECK_EQ(r.send(toneOp(0, FOXAPI_TONE_DCS, true, 0.0, 23, 2)).status(), FOXAPI_OK);   // target 0: the first radio (the focus)
    CHECK(r.until([&] { return r.radio(1).toneCode == 23u; }, 5000));
    CHECK_EQ(r.radio(1).tonePolarity, 2u);
    CHECK_EQ(r.radio(2).toneHz, 123.0);
    // the remover is its own, too
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_TONE_REMOVE, 2, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).toneRemove == 1u; }, 5000));
    CHECK_EQ(r.radio(1).toneRemove, 0u);
    // a radio that does not exist
    CHECK_EQ(r.send(toneOp(9, FOXAPI_TONE_OFF, false, 0.0, 0, 0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_TONE_REMOVE, 9, 0.0, 1)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.send(toneOp(FOXAPI_TARGET_ALL_RADIOS, FOXAPI_TONE_OFF, false, 0.0, 0, 0)).status(), FOXAPI_BAD_ARGUMENT);
}

void testCReachesThePipeline() {
    std::printf("C: the settings reach the radio's pipeline and the measurements come back\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 2, 0.0, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_SQUELCH, 2, -100.0)).status(), FOXAPI_OK);
    r.pump(20);
    CHECK_EQ(r.send(toneOp(2, FOXAPI_TONE_CTCSS, true, 100.0, 0, 0)).status(), FOXAPI_OK);
    auto& p2 = Access::pipeline(r.eng, 1);
    CHECK(r.until([&] { return p2.toneKind() == 1 && p2.toneHz() == 100.0; }, 5000));
    // the generator carries no CTCSS tone: the gate is shut and nothing is detected, and the state says so
    CHECK(r.until([&] { return r.radio(2).toneGate == 0u; }, 20000));
    CHECK_EQ(r.radio(2).toneDetectedHz, 0.0);
    CHECK_EQ(r.radio(2).audioGate, 0u);
    // the first radio is not touched
    CHECK_EQ(Access::pipeline(r.eng, 0).toneKind(), 0);
    CHECK_EQ(r.radio(1).toneGate, 1u);
    // off again: the condition is met again
    CHECK_EQ(r.send(toneOp(2, FOXAPI_TONE_OFF, false, 0.0, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).toneGate == 1u; }, 20000));
    CHECK_EQ(p2.toneKind(), 0);
    CHECK_EQ(r.radio(2).toneHz, 100.0);   // the choice is kept for next time
    // DCS and the remover reach their mirrors
    CHECK_EQ(r.send(toneOp(2, FOXAPI_TONE_DCS, true, 0.0, 754, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_TONE_REMOVE, 2, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return p2.toneKind() == 2 && p2.toneDcsDigits() == 754 && p2.tonePolarity() == 2 && p2.toneRemove(); }, 5000));
}

void testDRefusals() {
    std::printf("D: every refusal changes nothing\n");
    Rig r;
    CHECK_EQ(r.send(toneOp(1, FOXAPI_TONE_CTCSS, true, 88.5, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).toneKind == FOXAPI_TONE_CTCSS; }, 5000));
    const FoxReceiverState before = r.radio(1);
    struct Bad {
        int kind;
        double hz;
        int code;
        int pol;
        const char* why;
    };
    const Bad bad[] = {
        {3, 88.5, 0, 0, "a kind past DCS"},
        {-1, 88.5, 0, 0, "a negative kind"},
        {1, 99.0, 0, 0, "a tone that is not one of the 50"},
        {1, -67.0, 0, 0, "a negative tone"},
        {2, 0.0, 24, 0, "a code that is not one of the 83"},
        {2, 0.0, 19, 0, "a code written with a digit past 7"},
        {2, 0.0, 800, 0, "a code past 777"},
        {2, 0.0, -23, 0, "a negative code"},
        {2, 0.0, 23, 3, "a polarity past inverted"},
        {2, 0.0, 23, -1, "a negative polarity"},
    };
    for (const Bad& b : bad) {
        const testing::Outcome o = r.send(toneOp(1, b.kind, true, b.hz, b.code, b.pol));
        if (o.status() != FOXAPI_OUT_OF_RANGE) { std::printf("  %s answered %d\n", b.why, o.status()); }
        CHECK_EQ(o.status(), FOXAPI_OUT_OF_RANGE);
    }
    r.pump(10);
    const FoxReceiverState after = r.radio(1);
    CHECK_EQ(after.toneKind, before.toneKind);
    CHECK_EQ(after.toneHz, before.toneHz);
    CHECK_EQ(after.toneCode, before.toneCode);
    CHECK_EQ(after.tonePolarity, before.tonePolarity);
    CHECK_EQ(Access::pipeline(r.eng, 0).toneKind(), 1);
    CHECK_EQ(Access::pipeline(r.eng, 0).toneHz(), 88.5);
    // values not carried are not range-checked (the kind alone is); the radio's last choice is used
    CHECK_EQ(r.send(toneOp(1, FOXAPI_TONE_OFF, false, 99999.0, 99999, 99)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).toneKind == 0u; }, 5000));
    CHECK_EQ(r.radio(1).toneHz, 88.5);
    CHECK_EQ(r.send(toneOp(1, FOXAPI_TONE_CTCSS, false, 0.0, 0, 0)).status(), FOXAPI_OK);   // back on, with the kept tone
    CHECK(r.until([&] { return r.radio(1).toneKind == 1u; }, 5000));
    CHECK_EQ(r.radio(1).toneHz, 88.5);
    CHECK_EQ(Access::pipeline(r.eng, 0).toneHz(), 88.5);
    // the local (engine) path obeys the same rule: a value the API check would have passed on is still refused by the engine's own
    {
        const FoxCommandResult res = r.eng.applyCommand(toneOp(1, FOXAPI_TONE_CTCSS, true, 99.0, 0, 0), std::string());
        CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.radio(1).toneHz, 88.5);
    }
}

void testESavedAndRestored() {
    std::printf("E: saved per radio, restored in the engine and in its pipelines\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(toneOp(1, FOXAPI_TONE_CTCSS, true, 162.2, 0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(toneOp(2, FOXAPI_TONE_DCS, true, 0.0, 754, 2)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(a.aimed(FOXAPI_OP_SET_TONE_REMOVE, 2, 0.0, 1)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.toneSquelchKind, 1);
        CHECK_EQ(file.toneSquelchHz, 162.2);
        CHECK(!file.toneRemove);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].toneSquelchKind, 2);
        CHECK_EQ(file.radios[0].toneSquelchCode, 754);
        CHECK_EQ(file.radios[0].toneSquelchPolarity, 2);
        CHECK(file.radios[0].toneRemove);
        const json j = json::parse(ConfigStore::serialize(Access::snapshot(a.eng)));
        CHECK_EQ(j["toneSquelchKind"].get<int>(), 1);
        CHECK_EQ(j["radios"][0]["toneSquelchCode"].get<int>(), 754);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).toneKind, static_cast<std::uint32_t>(FOXAPI_TONE_CTCSS));
        CHECK_EQ(b.radio(1).toneHz, 162.2);
        CHECK_EQ(b.radio(2).toneKind, static_cast<std::uint32_t>(FOXAPI_TONE_DCS));
        CHECK_EQ(b.radio(2).toneCode, 754u);
        CHECK_EQ(b.radio(2).tonePolarity, 2u);
        CHECK_EQ(b.radio(2).toneRemove, 1u);
        CHECK_EQ(b.radio(1).toneRemove, 0u);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).toneKind() == 1 && Access::pipeline(b.eng, 0).toneHz() == 162.2; }, 5000));
        CHECK(b.until([&] { return Access::pipeline(b.eng, 1).toneKind() == 2 && Access::pipeline(b.eng, 1).toneDcsDigits() == 754 && Access::pipeline(b.eng, 1).toneRemove(); }, 5000));
        const AppConfig again = throughTheFile(b.eng);
        CHECK_EQ(again.toneSquelchKind, file.toneSquelchKind);
        CHECK_EQ(again.toneSquelchHz, file.toneSquelchHz);
        CHECK_EQ(again.radios[0].toneSquelchCode, file.radios[0].toneSquelchCode);
        CHECK_EQ(again.radios[0].toneRemove, file.radios[0].toneRemove);
    }
}

void testFProfiles() {
    std::printf("F: the page, CAT and TCI profiles do not carry the ops\n");
    for (const InternalKind k : {InternalKind::Web, InternalKind::Cat, InternalKind::Tci}) {
        for (const std::uint32_t op : cascade::engine::internalProfileOps(k)) {
            CHECK(op != FOXAPI_OP_SET_TONE_SQUELCH);
            CHECK(op != FOXAPI_OP_SET_TONE_REMOVE);
        }
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_tone\n");
    engine_api_test::hermetic("tone");
    Access::installHooks();
    testADefaults();
    testBEachRadioOwn();
    testCReachesThePipeline();
    testDRefusals();
    testESavedAndRestored();
    testFProfiles();
    return check::finish("test_engine_api_tone");
}
