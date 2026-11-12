// WHICH RADIO A COMMAND ADDRESSES (multi-radio engine, stage 3; docs/multi-radio-records/engine-stages-1-3.md, docs/API.md "Several radios").
//
// FoxCommand::target was documented ("object id where an op has several; 0 = default") and read nowhere. It is the radio's number now, for the ops
// that address ONE radio, under the focus rule that keeps every client that knows nothing of several radios working unchanged:
//
//     every API session has a FOCUSED radio (R1 until it says otherwise); a command that names no radio (target 0) means that session's focused radio;
//     a number 1..N names that radio; FOXAPI_TARGET_ALL_RADIOS is for RUN alone (Start together, STOP ALL); a number that names no radio the engine
//     has is refused FOXAPI_NO_DEVICE (the existing answer for "there is no radio"); every op that does not address one radio ignores the target.
//
// WHICH OPS ADDRESS ONE RADIO was settled by name, then by finding each op's case in Engine::applyCommand and checking that its body reaches the receiver's
// pipeline, its device, its mirrors, a recorder or the scanner (all 47 found; BOOKMARK_TUNE, which calls tuneToBookmark, and SET_SQUELCH_AUTO, which calls
// applySquelchAutoOp, read by hand; the six device ops answer NO_DEVICE of their own when no radio is open): the tuning ops, mode, bandwidth, squelch,
// volume and mute, the audio DSP, the display range, the source and every device control, the recorders, a bookmark's tune and the scanner (which tunes
// the receiver it is bound to). An op that is not listed acts on slot 0 as it always did; the list is the one place to add to when slots 1 and up carry
// more (stage 4). Read each op's handler once more when it is first given a second receiver to act on: the scan proves the handler touches the receiver,
// not that it touches nothing else.
//
// Pure: no engine, no session, no thread. The engine asks it with the session's focus and a predicate for "is there a radio with this number".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>

#include "core/app_commands.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {

inline bool isRadioScopedOp(std::uint32_t op) {
    switch (op) {
        // the receiver: run, tuning, demodulator, audio, display
        case FOXAPI_OP_RUN:
        case FOXAPI_OP_SET_CENTRE:
        case FOXAPI_OP_SET_FREQUENCY:
        case FOXAPI_OP_SET_VFO_OFFSET:
        case FOXAPI_OP_STEP_TUNE:
        case FOXAPI_OP_SET_MODE:
        case FOXAPI_OP_SET_BANDWIDTH:
        case FOXAPI_OP_SET_SQUELCH:
        case FOXAPI_OP_SET_VOLUME:
        case FOXAPI_OP_SET_MUTED:
        case FOXAPI_OP_SET_DEEMPHASIS:
        case FOXAPI_OP_SET_STEREO:
        case FOXAPI_OP_SET_NR:
        case FOXAPI_OP_SET_AGC:
        case FOXAPI_OP_SET_NB:
        case FOXAPI_OP_SET_TONE_SQUELCH:
        case FOXAPI_OP_SET_TONE_REMOVE:
        case FOXAPI_OP_SET_METER_UNITS:
        case FOXAPI_OP_SET_NR_METHOD:
        case FOXAPI_OP_SET_NOTCH_SLOT:
        case FOXAPI_OP_SET_CW_PITCH:
        case FOXAPI_OP_SET_NOISE_SQUELCH:
        case FOXAPI_OP_SET_EQ_BAND:
        case FOXAPI_OP_SET_AUDIO_FILTERS:
        case FOXAPI_OP_SET_SPECTRUM:
        case FOXAPP_OP_FILE_SEEK:
        case FOXAPP_OP_FILE_LOOP:
        case FOXAPI_OP_SET_IQ_CORRECT:
        case FOXAPI_OP_SET_AFC:
        case FOXAPI_OP_SET_CW_PEAK:
        case FOXAPI_OP_SET_STEREO_PLACE:
        case FOXAPP_OP_VFO_SELECT:
        case FOXAPP_OP_VFO_SWAP:
        case FOXAPP_OP_VFO_STORE:
        case FOXAPI_OP_SET_RECORD_FORMAT:
        case FOXAPI_OP_SET_CTUN:
        case FOXAPP_OP_SET_FM_WIDTH:
        case FOXAPI_OP_SET_FILTER_SHAPE:
        case FOXAPP_OP_MPX_RECORD:
        case FOXAPP_OP_IF_RECORD:
        case FOXAPP_OP_SET_IF_ZOOM:
        case FOXAPP_OP_PPM_MEASURE:
        // wire-b begin (the lane's per-radio operations; global ones are not listed)
        case FOXAPP_OP_FIELD_READ:
        case FOXAPP_OP_SIGMEAS_BANDWIDTH:
        case FOXAPP_OP_SIGMEAS_MODULATION:
        case FOXAPP_OP_SIGMEAS_SYMBOLS:
        case FOXAPP_OP_SIGMEAS_VSTIME:
        case FOXAPP_OP_SIGMEAS_BURSTS:
        case FOXAPP_OP_SIGMEAS_HOPCHIRP:
        case FOXAPP_OP_SIGMEAS_AUDIO:
        case FOXAPP_OP_SIGMEAS_DRIFT:
        case FOXAPP_OP_SIGMEAS_HISTOGRAM:
        case FOXAPP_OP_PSEUDO_DOPPLER:
        // wire-b end
        case FOXAPP_OP_CAL_MEASURE:
        case FOXAPP_OP_CAL_CLEAR:
        case FOXAPP_OP_RADIO_LOCK:
        case FOXAPP_OP_SET_STEREO_SYSTEM:
        case FOXAPI_OP_SET_NOTCH:
        case FOXAPI_OP_SET_AUTO_NOTCH:
        case FOXAPI_OP_SET_DISPLAY_RANGE:
        // the device behind it
        case FOXAPI_OP_SELECT_SOURCE:
        case FOXAPI_OP_SET_SAMPLE_RATE:
        case FOXAPI_OP_SET_GAIN:
        case FOXAPI_OP_SET_DEVICE_AGC:
        case FOXAPI_OP_SET_ANTENNA:
        case FOXAPI_OP_SET_BIAS_TEE:
        case FOXAPI_OP_SET_DEVICE_OPTION:
        // its recordings, the bookmark that tunes it, the scanner bound to it
        case FOXAPI_OP_RECORD_IQ:
        case FOXAPI_OP_RECORD_AUDIO:
        case FOXAPI_OP_BOOKMARK_TUNE:
        case FOXAPI_OP_SCANNER_RUN:
        case FOXAPI_OP_SCANNER_SKIP:
        case FOXAPI_OP_SCANNER_CONFIG:
        // the application's own ops that are the same controls (the window's vocabulary)
        case FOXAPP_OP_VFO_TO_ABSOLUTE:
        case FOXAPP_OP_SET_VFO_OFFSET_FREE:
        case FOXAPP_OP_SET_BANDWIDTH_STEP:
        case FOXAPP_OP_SET_BANDWIDTH_DRAG:
        case FOXAPP_OP_BANDWIDTH_DRAG_END:
        case FOXAPP_OP_SET_NR_STRENGTH:
        case FOXAPP_OP_SET_NOTCH_FREQUENCY:
        case FOXAPP_OP_SET_NOTCH_Q:
        case FOXAPP_OP_SET_DISPLAY_MIN:
        case FOXAPP_OP_SET_DISPLAY_MAX:
        case FOXAPP_OP_SET_GAIN_NO_READBACK:
        case FOXAPP_OP_SOUNDCARD_IQ_CENTRE:
        case FOXAPP_OP_AIRSPY_DECIMATION:
        case FOXAPP_OP_AIRSPY_GAIN_MODE:
        case FOXAPP_OP_AIRSPY_AGC:
        case FOXAPP_OP_SET_PPM:
        case FOXAPP_OP_SET_SQUELCH_AUTO:
        case FOXAPP_OP_SCANNER_RANGE:
            return true;
        default:
            return false;
    }
}

struct RadioPick {
    std::int32_t status = FOXAPI_OK;  // FOXAPI_OK, FOXAPI_NO_DEVICE (no such radio) or FOXAPI_BAD_ARGUMENT (ALL for an op that is not RUN)
    // The radio the command is for, from 1; FOXAPI_TARGET_ALL_RADIOS for RUN with that target; the focus for an op that does not address a radio
    // (nothing reads it then).
    std::uint32_t radio = 1;
};

// `focus`: the sender's focused radio (1 for a sender with no session of its own). `live(n)`: whether the engine has a radio numbered n (false for
// n = 0 and for any n past the engine's limit).
template <class Live>
RadioPick pickRadio(std::uint32_t op, std::uint32_t target, std::uint32_t focus, Live&& live) {
    RadioPick p;
    p.radio = focus;
    if (!isRadioScopedOp(op)) { return p; }
    if (target == FOXAPI_TARGET_ALL_RADIOS) {
        if (op != FOXAPI_OP_RUN) { p.status = FOXAPI_BAD_ARGUMENT; return p; }
        p.radio = FOXAPI_TARGET_ALL_RADIOS;
        return p;
    }
    p.radio = target == 0u ? focus : target;
    if (!live(p.radio)) { p.status = FOXAPI_NO_DEVICE; }
    return p;
}

// A focus change moves every seq group of the session's state by `by`: the groups are counters the interface compares with the last it read, and they
// only ever go up, so adding to all of them makes each differ from the last read whatever else the engine published meanwhile.
inline void bumpSeqs(FoxReceiverState& st, std::uint64_t by) {
    st.seq += by;
    st.tuneSeq += by;
    st.modeSeq += by;
    st.deviceSeq += by;
    st.audioSeq += by;
    st.displaySeq += by;
    st.txSeq += by;
    st.listSeq += by;
    st.rdsSeq += by;
    st.scanSeq += by;
}

}  // namespace cascade::engine
