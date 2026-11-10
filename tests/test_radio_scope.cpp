// test_radio_scope.cpp - which radio a command addresses (engine/radio_scope.hpp, multi-radio stage 3): the target rule and the focus bump, as pure functions.
//
//   A  THE RULE: an op that addresses one radio takes target 0 as the sender's focus and 1..N as that radio; a number that names no radio is NO_DEVICE;
//      FOXAPI_TARGET_ALL_RADIOS is for RUN alone (BAD_ARGUMENT for any other scoped op); an op that does not address a radio ignores the target whatever
//      it says (even a number that names no radio, even ALL);
//   B  THE LIST: the ops the engine treats as addressing one radio are exactly the ones named here, and the ones that are global (bookmarks, plugins, the
//      store, the patch, the transmitter, settings, reports) are not - so a command that is not about a radio is never refused for its target;
//   C  THE FOCUS BUMP: a change of focus moves every seq group of the state by the same amount, and a bump of zero moves none.
//
// Each was shown red against a mutant of the header (the rule answering OK for every target; the list missing an op; a seq group left out of the bump).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/radio_scope.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {

using cascade::engine::bumpSeqs;
using cascade::engine::isRadioScopedOp;
using cascade::engine::pickRadio;
using cascade::engine::RadioPick;

// An engine with `count` radios, numbered from 1.
auto has(std::uint32_t count) {
    return [count](std::uint32_t n) { return n >= 1u && n <= count; };
}

void testTheRule() {
    // One radio: 0 is the focus, 1 is radio 1, 2 is nothing.
    RadioPick p = pickRadio(FOXAPI_OP_SET_VOLUME, 0u, 1u, has(1));
    CHECK_EQ(p.status, FOXAPI_OK);
    CHECK_EQ(p.radio, 1u);
    p = pickRadio(FOXAPI_OP_SET_VOLUME, 1u, 1u, has(1));
    CHECK_EQ(p.status, FOXAPI_OK);
    CHECK_EQ(p.radio, 1u);
    p = pickRadio(FOXAPI_OP_SET_VOLUME, 2u, 1u, has(1));
    CHECK_EQ(p.status, FOXAPI_NO_DEVICE);
    p = pickRadio(FOXAPI_OP_SET_VOLUME, 9u, 1u, has(1));
    CHECK_EQ(p.status, FOXAPI_NO_DEVICE);
    // Four radios, the sender's focus on 3: 0 means 3, a number means itself.
    p = pickRadio(FOXAPI_OP_SET_FREQUENCY, 0u, 3u, has(4));
    CHECK_EQ(p.status, FOXAPI_OK);
    CHECK_EQ(p.radio, 3u);
    p = pickRadio(FOXAPI_OP_SET_FREQUENCY, 2u, 3u, has(4));
    CHECK_EQ(p.status, FOXAPI_OK);
    CHECK_EQ(p.radio, 2u);
    p = pickRadio(FOXAPI_OP_SET_FREQUENCY, 5u, 3u, has(4));
    CHECK_EQ(p.status, FOXAPI_NO_DEVICE);
    // A focus on a radio that is gone is NO_DEVICE too (target 0 is the focus).
    p = pickRadio(FOXAPI_OP_SET_FREQUENCY, 0u, 3u, has(2));
    CHECK_EQ(p.status, FOXAPI_NO_DEVICE);
    // Eight, the engine's limit: radio 8 is a radio, 9 is not. Nothing is four.
    p = pickRadio(FOXAPI_OP_SET_FREQUENCY, 8u, 1u, has(8));
    CHECK_EQ(p.status, FOXAPI_OK);
    CHECK_EQ(p.radio, 8u);
    p = pickRadio(FOXAPI_OP_SET_FREQUENCY, 9u, 1u, has(8));
    CHECK_EQ(p.status, FOXAPI_NO_DEVICE);
    // ALL: RUN alone.
    p = pickRadio(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS, 1u, has(1));
    CHECK_EQ(p.status, FOXAPI_OK);
    CHECK_EQ(p.radio, FOXAPI_TARGET_ALL_RADIOS);
    p = pickRadio(FOXAPI_OP_SET_VOLUME, FOXAPI_TARGET_ALL_RADIOS, 1u, has(4));
    CHECK_EQ(p.status, FOXAPI_BAD_ARGUMENT);
    p = pickRadio(FOXAPI_OP_SELECT_SOURCE, FOXAPI_TARGET_ALL_RADIOS, 1u, has(4));
    CHECK_EQ(p.status, FOXAPI_BAD_ARGUMENT);
    // An op that is not about a radio ignores the target altogether.
    for (const std::uint32_t target : {0u, 1u, 2u, 99u, FOXAPI_TARGET_ALL_RADIOS}) {
        p = pickRadio(FOXAPI_OP_BOOKMARK_ADD, target, 1u, has(1));
        CHECK_EQ(p.status, FOXAPI_OK);
        p = pickRadio(FOXAPI_OP_TX_PTT, target, 1u, has(1));
        CHECK_EQ(p.status, FOXAPI_OK);
        p = pickRadio(FOXAPI_OP_STORE_FETCH, target, 1u, has(1));
        CHECK_EQ(p.status, FOXAPI_OK);
    }
}

void testTheList() {
    const std::uint32_t scoped[] = {
        FOXAPI_OP_RUN, FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_FREQUENCY, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_STEP_TUNE, FOXAPI_OP_SET_MODE,
        FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_SQUELCH, FOXAPI_OP_SET_VOLUME, FOXAPI_OP_SET_MUTED, FOXAPI_OP_SET_DEEMPHASIS, FOXAPI_OP_SET_STEREO,
        FOXAPI_OP_SET_NR, FOXAPI_OP_SET_NOTCH, FOXAPI_OP_SET_AUTO_NOTCH, FOXAPI_OP_SET_DISPLAY_RANGE, FOXAPI_OP_SELECT_SOURCE,
        FOXAPI_OP_SET_SAMPLE_RATE, FOXAPI_OP_SET_GAIN, FOXAPI_OP_SET_DEVICE_AGC, FOXAPI_OP_SET_ANTENNA, FOXAPI_OP_SET_BIAS_TEE,
        FOXAPI_OP_SET_DEVICE_OPTION, FOXAPI_OP_RECORD_IQ, FOXAPI_OP_RECORD_AUDIO, FOXAPI_OP_BOOKMARK_TUNE, FOXAPI_OP_SCANNER_RUN,
        FOXAPI_OP_SCANNER_SKIP, FOXAPI_OP_SCANNER_CONFIG, FOXAPP_OP_VFO_TO_ABSOLUTE, FOXAPP_OP_SET_VFO_OFFSET_FREE, FOXAPP_OP_SET_BANDWIDTH_STEP,
        FOXAPP_OP_SET_BANDWIDTH_DRAG, FOXAPP_OP_BANDWIDTH_DRAG_END, FOXAPP_OP_SET_NR_STRENGTH, FOXAPP_OP_SET_NOTCH_FREQUENCY, FOXAPP_OP_SET_NOTCH_Q,
        FOXAPP_OP_SET_DISPLAY_MIN, FOXAPP_OP_SET_DISPLAY_MAX, FOXAPP_OP_SET_GAIN_NO_READBACK, FOXAPP_OP_SOUNDCARD_IQ_CENTRE,
        FOXAPP_OP_AIRSPY_DECIMATION, FOXAPP_OP_AIRSPY_GAIN_MODE, FOXAPP_OP_AIRSPY_AGC, FOXAPP_OP_SET_PPM, FOXAPP_OP_SET_SQUELCH_AUTO,
        FOXAPP_OP_SCANNER_RANGE,
    };
    for (const std::uint32_t op : scoped) { CHECK(isRadioScopedOp(op)); }
    CHECK_EQ(sizeof(scoped) / sizeof(scoped[0]), static_cast<std::size_t>(47));
    const std::uint32_t global[] = {
        FOXAPI_OP_SET_BAND_FOLLOW, FOXAPI_OP_SET_BAND_PLAN, FOXAPI_OP_SCAN_DEVICES, FOXAPI_OP_BOOKMARK_ADD, FOXAPI_OP_BOOKMARK_REMOVE,
        FOXAPI_OP_BOOKMARK_FAVOURITE, FOXAPI_OP_BOOKMARK_IMPORT, FOXAPI_OP_BOOKMARK_REMOVE_GROUP, FOXAPI_OP_DECODER_START, FOXAPI_OP_DECODER_STOP,
        FOXAPI_OP_DECODER_STOP_ALL, FOXAPI_OP_PLUGIN_PRESET, FOXAPI_OP_STORE_FETCH, FOXAPI_OP_STORE_INSTALL, FOXAPI_OP_PATCH_LOAD, FOXAPI_OP_PATCH_RUN,
        FOXAPI_OP_PATCH_ALL_OFF, FOXAPI_OP_TX_OPEN, FOXAPI_OP_TX_CLOSE, FOXAPI_OP_TX_PTT, FOXAPI_OP_TX_LATCH, FOXAPI_OP_TX_SET_FREQUENCY,
        FOXAPI_OP_SETTING_SET, FOXAPI_OP_PROBLEM_REPORT, FOXAPI_OP_TELEMETRY_ENABLE, FOXAPI_OP_AUDIO_DEVICE, FOXAPI_OP_SET_POSITION, FOXAPI_OP_GPS,
        FOXAPI_OP_SERVER_CONFIG, FOXAPI_OP_UPDATE_CHECK, FOXAPI_OP_RADIO_FOCUS, FOXAPP_OP_SET_CONVERTER, FOXAPP_OP_MARKER_DROP,
        FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_OP_AIRBAND, 0x7FFFu,
    };
    for (const std::uint32_t op : global) { CHECK(!isRadioScopedOp(op)); }
}

void testTheFocusBump() {
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    st.seq = 10;
    st.tuneSeq = 11;
    st.modeSeq = 12;
    st.deviceSeq = 13;
    st.audioSeq = 14;
    st.displaySeq = 15;
    st.txSeq = 16;
    st.listSeq = 17;
    st.rdsSeq = 18;
    st.scanSeq = 19;
    FoxReceiverState same = st;
    bumpSeqs(same, 0);
    CHECK(same.seq == 10 && same.tuneSeq == 11 && same.modeSeq == 12 && same.deviceSeq == 13 && same.audioSeq == 14);
    CHECK(same.displaySeq == 15 && same.txSeq == 16 && same.listSeq == 17 && same.rdsSeq == 18 && same.scanSeq == 19);
    bumpSeqs(st, 3);
    CHECK_EQ(st.seq, 13ull);
    CHECK_EQ(st.tuneSeq, 14ull);
    CHECK_EQ(st.modeSeq, 15ull);
    CHECK_EQ(st.deviceSeq, 16ull);
    CHECK_EQ(st.audioSeq, 17ull);
    CHECK_EQ(st.displaySeq, 18ull);
    CHECK_EQ(st.txSeq, 19ull);
    CHECK_EQ(st.listSeq, 20ull);
    CHECK_EQ(st.rdsSeq, 21ull);
    CHECK_EQ(st.scanSeq, 22ull);
}

}  // namespace

int main() {
    std::printf("test_radio_scope\n");
    testTheRule();
    testTheList();
    testTheFocusBump();
    return testSummary("test_radio_scope");
}
