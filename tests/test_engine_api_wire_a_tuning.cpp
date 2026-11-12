// test_engine_api_wire_a_tuning.cpp - THE TUNING GROUP OF WIRING LANE wire-a, held against the real engine through its API (no window): the models are in src/core
// (their own tests hold them alone); what is held HERE is what the engine does with them.
//
//   H1  FREQUENCY HISTORY: a retune from any source (here SET_FREQUENCY) is recorded once a pass; HISTORY_STEP back and forward retune the radio and its mode; an end
//       of the list is NO_CHANGE; a bad direction is OUT_OF_RANGE; the step is not recorded as a new entry; facts say where the list is.
//
//   C1  CHANNEL MODE: choose a plan, the radio lands on channel 1 in the plan's mode and width; next / previous wrap; go to a label; a marine channel with two
//       frequencies shows both; leaving; the refusals (unknown plan, unknown label, no plan chosen, bad ival).
//
//   K1  KEY ACTIONS: bandwidth up / down walk the mode's list and stop at its ends; cycle_mode goes round; next / previous bookmark wrap; centre_signal moves the tuned frequency onto a generated carrier
//       20 kHz away inside the passband and then says it is centred; cycle_step is the window's; any other id is refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>

#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

FoxCommand setFreq(double hz) { return testing::cmd(FOXAPI_OP_SET_FREQUENCY, hz, 0); }
FoxCommand step(int dir) { return testing::cmd(FOXAPP_OP_HISTORY_STEP, 0.0, dir); }

void testHistory() {
    std::printf("H1: frequency history\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("history.1.count"), std::string("1"));   // the first frequency the radio has is the first entry
    // Three retunes a pause apart (the history joins a dial spin: 1.5 s) make three more entries.
    const double hz[3] = {100.1e6, 98.0e6, 91.3e6};
    for (const double f : hz) {
        CHECK_EQ(r.send(setFreq(f)).status(), FOXAPI_OK);
        r.settle(1700);
    }
    CHECK_EQ(r.fact("history.1.count"), std::string("4"));
    CHECK_EQ(r.fact("history.1.position"), std::string("3"));
    CHECK_EQ(r.fact("history.1.canBack"), std::string("1"));
    CHECK_EQ(r.fact("history.1.canForward"), std::string("0"));
    // forward at the end of the list changes nothing
    CHECK_EQ(r.send(step(1)).status(), FOXAPI_NO_CHANGE);
    // back twice: 98.0 MHz, then 100.1 MHz; the step is not recorded as a retune
    testing::Outcome o = r.send(step(0));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK(std::fabs(o.result.applied[0] - 98.0e6) < 1.0);
    r.settle(300);
    CHECK_EQ(r.fact("history.1.count"), std::string("4"));
    CHECK_EQ(r.fact("history.1.position"), std::string("2"));
    o = r.send(step(0));
    CHECK(std::fabs(o.result.applied[0] - 100.1e6) < 1.0);
    CHECK_EQ(r.fact("history.1.position"), std::string("1"));
    // forward again
    o = r.send(step(1));
    CHECK(std::fabs(o.result.applied[0] - 98.0e6) < 1.0);
    CHECK_EQ(r.fact("history.1.canForward"), std::string("1"));
    // a retune now throws the entries ahead away
    CHECK_EQ(r.send(setFreq(87.5e6)).status(), FOXAPI_OK);
    r.settle(300);
    CHECK_EQ(r.fact("history.1.canForward"), std::string("0"));
    CHECK_EQ(r.fact("history.1.count"), std::string("4"));   // [start, 100.1, 98.0, 87.5]
    // failure path: a direction that is neither
    CHECK_EQ(r.send(step(2)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(step(-1)).status(), FOXAPI_OUT_OF_RANGE);
    // a radio that is not there
    FoxCommand c = step(0);
    c.target = 5;
    CHECK_EQ(r.send(c).status(), FOXAPI_NO_DEVICE);
}

FoxCommand chan(int what, const char* text = nullptr) { return testing::cmd(FOXAPP_OP_CHANNEL, 0.0, what, text); }

void testChannels() {
    std::printf("C1: channel mode\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(chan(1)).status(), FOXAPI_NO_CHANGE);               // no plan chosen
    CHECK_EQ(r.send(chan(4)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(chan(0, "no-such-plan")).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(chan(9)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK(r.fact("channel.plans").find("pmr446-analogue") != std::string::npos);
    CHECK(r.fact("channel.plans").find("marine-vhf-itu") != std::string::npos);
    // PMR446 analogue: channel 1 is 446.00625 MHz, NFM, 12.5 kHz
    testing::Outcome o = r.send(chan(0, "pmr446-analogue"));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK(std::fabs(o.result.applied[0] - 446.00625e6) < 2.0);
    CHECK_EQ(r.fact("channel.1.label"), std::string("1"));
    CHECK_EQ(r.fact("channel.1.modeHint"), std::string("NFM"));
    CHECK_EQ(r.fact("tuned.1.mode"), std::string("NFM"));
    CHECK_EQ(r.fact("tuned.1.bandwidthHz"), std::string("12500"));
    // next: channel 2 at +12.5 kHz; previous twice wraps to channel 16
    o = r.send(chan(1));
    CHECK(std::fabs(o.result.applied[0] - 446.01875e6) < 2.0);
    CHECK_EQ(r.fact("channel.1.label"), std::string("2"));
    CHECK_EQ(r.send(chan(2)).status(), FOXAPI_OK);
    o = r.send(chan(2));
    CHECK_EQ(r.fact("channel.1.label"), std::string("16"));
    CHECK(std::fabs(o.result.applied[0] - (446.00625e6 + 15 * 12500.0)) < 2.0);
    // go to a label, and a label the plan does not have
    CHECK_EQ(r.send(chan(3, "7")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("channel.1.label"), std::string("7"));
    CHECK_EQ(r.send(chan(3, "99")).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.fact("channel.1.label"), std::string("7"));              // a refusal changes nothing
    // CB is AM at 10 kHz; marine channel 01 has two frequencies (ship 156.050, coast 160.650)
    CHECK_EQ(r.send(chan(0, "cb-cept-40")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("tuned.1.mode"), std::string("AM"));
    CHECK_EQ(r.send(chan(0, "marine-vhf-itu")).status(), FOXAPI_OK);
    CHECK_EQ(r.send(chan(3, "01")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("channel.1.freqHz"), std::string("156050000"));
    CHECK_EQ(r.fact("channel.1.altHz"), std::string("160650000"));
    // leaving keeps the radio where it is
    const std::string where = r.fact("tuned.1.hz");
    CHECK_EQ(r.send(chan(4)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("channel.1.plan"), std::string());
    CHECK_EQ(r.fact("tuned.1.hz"), where);
}

FoxCommand key(const char* id) { return testing::cmd(FOXAPP_OP_KEY_ACTION, 0.0, 0, id); }

void testKeys() {
    std::printf("K1: key actions\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(key("nonsense")).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(key("cycle_step")).status(), FOXAPI_UNSUPPORTED);
    // bandwidth: NFM list 5000 6250 7500 10000 12500 15000 25000
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 12500.0)).status(), FOXAPI_OK);
    testing::Outcome o = r.send(key("bandwidth_up"));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(r.fact("tuned.1.bandwidthHz"), std::string("15000"));
    CHECK_EQ(r.send(key("bandwidth_up")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("tuned.1.bandwidthHz"), std::string("25000"));
    CHECK_EQ(r.send(key("bandwidth_up")).status(), FOXAPI_NO_CHANGE);   // the widest: no wrap
    CHECK_EQ(r.fact("tuned.1.bandwidthHz"), std::string("25000"));
    CHECK_EQ(r.send(key("bandwidth_down")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("tuned.1.bandwidthHz"), std::string("15000"));
    // mode: NFM -> WFM -> AM
    CHECK_EQ(r.send(key("cycle_mode")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("tuned.1.mode"), std::string("WFM"));
    CHECK_EQ(r.send(key("cycle_mode")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("tuned.1.mode"), std::string("AM"));
    // a mode with no list (DSB): NO_CHANGE
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_DSB)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(key("bandwidth_up")).status(), FOXAPI_NO_CHANGE);
    // bookmarks: three, next / previous wrap
    const double marks[3] = {100.0e6, 101.0e6, 102.0e6};
    for (const double m : marks) {
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, m, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "m")).status(), FOXAPI_OK);
    }
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.5e6, 0)).status(), FOXAPI_OK);
    o = r.send(key("next_bookmark"));
    CHECK(std::fabs(o.result.applied[0] - 101.0e6) < 2.0);
    o = r.send(key("next_bookmark"));
    CHECK(std::fabs(o.result.applied[0] - 102.0e6) < 2.0);
    o = r.send(key("next_bookmark"));
    CHECK(std::fabs(o.result.applied[0] - 100.0e6) < 2.0);   // wraps
    o = r.send(key("previous_bookmark"));
    CHECK(std::fabs(o.result.applied[0] - 102.0e6) < 2.0);   // wraps back
    // centre_signal: a carrier 20 kHz above the tuned frequency, inside a 150 kHz WFM passband
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CTUN, 0.0, 1)).status(), FOXAPI_OK);   // the tune moves the VFO, not the generator's centre: the carrier stays where it is
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6, 0)).status(), FOXAPI_OK);
    const double tunedNow = std::strtod(r.fact("tuned.1.hz").c_str(), nullptr), centreNow = std::strtod(r.fact("tuned.1.centreHz").c_str(), nullptr);
    CHECK(Access::genTone(r.eng, 0, tunedNow + 20.0e3 - centreNow, -10.0f));   // 20 kHz above where the radio is, whatever its VFO offset
    CHECK(Access::genTone(r.eng, 1, 1.0e6, -90.0f));   // the demo's second tone, out of the way and quiet
    r.settle(600);
    o = r.send(key("centre_signal"));
    std::printf("  centre_signal: %d, tuned %.0f Hz (the carrier is at +20000)\n", o.status(), o.result.applied[0] - 100.0e6);
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK(std::fabs(o.result.applied[0] - (tunedNow + 20.0e3)) < 4000.0);   // onto the carrier, to within a few spectrum bins
    CHECK_EQ(r.send(key("centre_signal")).status(), FOXAPI_NO_CHANGE);   // centred already (the carrier is fixed: the radio is on centre tuning)
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_tuning");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testHistory();
    testChannels();
    testKeys();
    return check::finish("test_engine_api_wire_a_tuning");
}
