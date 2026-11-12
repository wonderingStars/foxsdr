// test_engine_api_wire_a_tuning.cpp - THE TUNING GROUP OF WIRING LANE wire-a, held against the real engine through its API (no window): the models are in src/core
// (their own tests hold them alone); what is held HERE is what the engine does with them.
//
//   H1  FREQUENCY HISTORY: a retune from any source (here SET_FREQUENCY) is recorded once a pass; HISTORY_STEP back and forward retune the radio and its mode; an end
//       of the list is NO_CHANGE; a bad direction is OUT_OF_RANGE; the step is not recorded as a new entry; facts say where the list is.
//
//   C1  CHANNEL MODE: choose a plan, the radio lands on channel 1 in the plan's mode and width; next / previous wrap; go to a label; a marine channel with two
//       frequencies shows both; leaving; the refusals (unknown plan, unknown label, no plan chosen, bad ival).
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

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_tuning");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testHistory();
    testChannels();
    return check::finish("test_engine_api_wire_a_tuning");
}
