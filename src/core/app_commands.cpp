// app_commands.cpp - op names and the list of ops AppWindow::applyCommand
// implements. See app_commands.hpp and docs/engine-stage1.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/app_commands.hpp"

namespace cascade::core::cmd {

namespace {

struct OpEntry {
    std::uint32_t op;
    const char* name;
    bool implemented;  // applyCommand has a case for it (else UNSUPPORTED)
};

// EVERY op in foxsdr_api.h 0.2 and every extension, in header order. The
// `implemented` column is what isKnownOp answers and what tests/
// test_apply_command.cpp requires a case for: an op marked true here with no
// test fails that test, and so does a test for an op marked false.
constexpr OpEntry kOps[] = {
    {FOXAPI_OP_RUN, "RUN", true},
    {FOXAPI_OP_SET_CENTRE, "SET_CENTRE", true},
    {FOXAPI_OP_SET_FREQUENCY, "SET_FREQUENCY", true},
    {FOXAPI_OP_SET_VFO_OFFSET, "SET_VFO_OFFSET", true},
    {FOXAPI_OP_STEP_TUNE, "STEP_TUNE", true},
    {FOXAPI_OP_SET_MODE, "SET_MODE", true},
    {FOXAPI_OP_SET_BANDWIDTH, "SET_BANDWIDTH", true},
    {FOXAPI_OP_SET_SQUELCH, "SET_SQUELCH", true},
    {FOXAPI_OP_SET_VOLUME, "SET_VOLUME", true},
    {FOXAPI_OP_SET_MUTED, "SET_MUTED", true},
    {FOXAPI_OP_SET_DEEMPHASIS, "SET_DEEMPHASIS", true},
    {FOXAPI_OP_SET_STEREO, "SET_STEREO", true},
    {FOXAPI_OP_SET_NR, "SET_NR", true},
    {FOXAPI_OP_SET_NOTCH, "SET_NOTCH", true},
    {FOXAPI_OP_SET_AUTO_NOTCH, "SET_AUTO_NOTCH", true},
    {FOXAPI_OP_SET_DISPLAY_RANGE, "SET_DISPLAY_RANGE", true},
    {FOXAPI_OP_SET_BAND_PLAN, "SET_BAND_PLAN", true},
    {FOXAPI_OP_SCAN_DEVICES, "SCAN_DEVICES", true},
    {FOXAPI_OP_SELECT_SOURCE, "SELECT_SOURCE", true},
    {FOXAPI_OP_SET_SAMPLE_RATE, "SET_SAMPLE_RATE", true},
    {FOXAPI_OP_SET_GAIN, "SET_GAIN", true},
    {FOXAPI_OP_SET_DEVICE_AGC, "SET_DEVICE_AGC", true},
    {FOXAPI_OP_SET_ANTENNA, "SET_ANTENNA", true},
    {FOXAPI_OP_SET_BIAS_TEE, "SET_BIAS_TEE", true},
    {FOXAPI_OP_SET_DEVICE_OPTION, "SET_DEVICE_OPTION", true},
    {FOXAPI_OP_RECORD_IQ, "RECORD_IQ", true},
    {FOXAPI_OP_RECORD_AUDIO, "RECORD_AUDIO", true},
    {FOXAPI_OP_BOOKMARK_ADD, "BOOKMARK_ADD", true},
    {FOXAPI_OP_BOOKMARK_TUNE, "BOOKMARK_TUNE", true},
    {FOXAPI_OP_BOOKMARK_REMOVE, "BOOKMARK_REMOVE", true},
    {FOXAPI_OP_BOOKMARK_FAVOURITE, "BOOKMARK_FAVOURITE", true},
    {FOXAPI_OP_BOOKMARK_IMPORT, "BOOKMARK_IMPORT", false},
    {FOXAPI_OP_BOOKMARK_REMOVE_GROUP, "BOOKMARK_REMOVE_GROUP", true},
    {FOXAPI_OP_SCANNER_RUN, "SCANNER_RUN", true},
    {FOXAPI_OP_SCANNER_SKIP, "SCANNER_SKIP", true},
    {FOXAPI_OP_SCANNER_CONFIG, "SCANNER_CONFIG", true},
    {FOXAPI_OP_DECODER_START, "DECODER_START", true},
    {FOXAPI_OP_DECODER_STOP, "DECODER_STOP", true},
    {FOXAPI_OP_DECODER_STOP_ALL, "DECODER_STOP_ALL", true},
    {FOXAPI_OP_PLUGIN_PRESET, "PLUGIN_PRESET", true},
    {FOXAPI_OP_PLUGIN_GRANT, "PLUGIN_GRANT", true},
    {FOXAPI_OP_PLUGIN_COMMAND, "PLUGIN_COMMAND", true},
    {FOXAPI_OP_PLUGIN_MUTE, "PLUGIN_MUTE", true},
    {FOXAPI_OP_PLUGIN_RESCAN, "PLUGIN_RESCAN", true},
    {FOXAPI_OP_USER_PRESET_SAVE, "USER_PRESET_SAVE", true},
    {FOXAPI_OP_USER_PRESET_FORGET, "USER_PRESET_FORGET", false},
    {FOXAPI_OP_STORE_FETCH, "STORE_FETCH", true},
    {FOXAPI_OP_STORE_INSTALL, "STORE_INSTALL", true},
    {FOXAPI_OP_STORE_REMOVE, "STORE_REMOVE", true},
    {FOXAPI_OP_STORE_CANCEL, "STORE_CANCEL", true},
    {FOXAPI_OP_STORE_UPDATE_ALL, "STORE_UPDATE_ALL", true},
    {FOXAPI_OP_PATCH_LOAD, "PATCH_LOAD", false},
    {FOXAPI_OP_PATCH_RUN, "PATCH_RUN", true},
    {FOXAPI_OP_PATCH_ALL_OFF, "PATCH_ALL_OFF", true},
    {FOXAPI_OP_TX_OPEN, "TX_OPEN", true},
    {FOXAPI_OP_TX_CLOSE, "TX_CLOSE", true},
    {FOXAPI_OP_TX_PTT, "TX_PTT", true},
    {FOXAPI_OP_TX_LATCH, "TX_LATCH", false},
    {FOXAPI_OP_TX_SET_MODE, "TX_SET_MODE", true},
    {FOXAPI_OP_TX_SET_FREQUENCY, "TX_SET_FREQUENCY", true},
    {FOXAPI_OP_TX_SET_POWER, "TX_SET_POWER", true},
    {FOXAPI_OP_TX_SET_INPUT, "TX_SET_INPUT", true},
    {FOXAPI_OP_TX_SET_SPLIT, "TX_SET_SPLIT", true},
    {FOXAPI_OP_TX_SET_TONE, "TX_SET_TONE", true},
    {FOXAPI_OP_TX_SET_MONITOR, "TX_SET_MONITOR", true},
    {FOXAPI_OP_TX_REMOTE_ARM, "TX_REMOTE_ARM", false},
    {FOXAPI_OP_SETTING_SET, "SETTING_SET", false},
    {FOXAPI_OP_PROBLEM_REPORT, "PROBLEM_REPORT", false},
    {FOXAPI_OP_FEATURE_REQUEST, "FEATURE_REQUEST", false},
    {FOXAPI_OP_TELEMETRY_ENABLE, "TELEMETRY_ENABLE", true},
    {FOXAPI_OP_AUDIO_DEVICE, "AUDIO_DEVICE", true},
    {FOXAPI_OP_SET_POSITION, "SET_POSITION", true},
    {FOXAPI_OP_GPS, "GPS", true},
    {FOXAPI_OP_SERVER_CONFIG, "SERVER_CONFIG", false},
    {FOXAPI_OP_UPDATE_CHECK, "UPDATE_CHECK", false},
    // --- extensions ---
    {FOXAPP_OP_VFO_TO_ABSOLUTE, "APP_VFO_TO_ABSOLUTE", true},
    {FOXAPP_OP_SET_VFO_OFFSET_FREE, "APP_SET_VFO_OFFSET_FREE", true},
    {FOXAPP_OP_SET_BANDWIDTH_STEP, "APP_SET_BANDWIDTH_STEP", true},
    {FOXAPP_OP_SET_BANDWIDTH_DRAG, "APP_SET_BANDWIDTH_DRAG", true},
    {FOXAPP_OP_SET_NR_STRENGTH, "APP_SET_NR_STRENGTH", true},
    {FOXAPP_OP_SET_NOTCH_FREQUENCY, "APP_SET_NOTCH_FREQUENCY", true},
    {FOXAPP_OP_SET_NOTCH_Q, "APP_SET_NOTCH_Q", true},
    {FOXAPP_OP_SET_DISPLAY_MIN, "APP_SET_DISPLAY_MIN", true},
    {FOXAPP_OP_SET_DISPLAY_MAX, "APP_SET_DISPLAY_MAX", true},
    {FOXAPP_OP_SCAN_DEVICES_ON_OPEN, "APP_SCAN_DEVICES_ON_OPEN", true},
    {FOXAPP_OP_SET_NETWORK_USRP_SCAN, "APP_SET_NETWORK_USRP_SCAN", true},
    {FOXAPP_OP_PATCH_RADIO_LIST_OPENED, "APP_PATCH_RADIO_LIST_OPENED", true},
    {FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, "APP_SET_TRANSMIT_PAGE_OPEN", true},
    {FOXAPP_OP_SET_SOURCE_ERROR, "APP_SET_SOURCE_ERROR", true},
    {FOXAPP_OP_SET_BOOKMARK_NOTE, "APP_SET_BOOKMARK_NOTE", true},
    {FOXAPP_OP_CLEAR_STATUS, "APP_CLEAR_STATUS", true},
    {FOXAPP_OP_SET_CATALOGUE_URL, "APP_SET_CATALOGUE_URL", true},
    {FOXAPP_OP_SOUND_CARD_FORM, "APP_SOUND_CARD_FORM", true},
    {FOXAPP_OP_PATCH_LOOK_FOR_RADIOS, "APP_PATCH_LOOK_FOR_RADIOS", true},
    {FOXAPP_OP_MUTE_KEEP_RUNNING, "APP_MUTE_KEEP_RUNNING", true},
    {FOXAPP_OP_AIRSPY_DECIMATION, "APP_AIRSPY_DECIMATION", true},
    {FOXAPP_OP_AIRSPY_GAIN_MODE, "APP_AIRSPY_GAIN_MODE", true},
    {FOXAPP_OP_AIRSPY_AGC, "APP_AIRSPY_AGC", true},
    {FOXAPP_OP_SET_PLUTO_URI, "APP_SET_PLUTO_URI", true},
    {FOXAPP_OP_SET_TRANSMIT_ARGS, "APP_SET_TRANSMIT_ARGS", true},
    {FOXAPP_OP_TELEMETRY_CONSENT, "APP_TELEMETRY_CONSENT", true},
    {FOXAPP_OP_PATCH_SET_GRAPH, "APP_PATCH_SET_GRAPH", true},
    {FOXAPP_OP_SET_GAIN_NO_READBACK, "APP_SET_GAIN_NO_READBACK", true},
    {FOXAPP_OP_SET_CONVERTER, "APP_SET_CONVERTER", true},
    {FOXAPP_OP_SOUNDCARD_IQ_CENTRE, "APP_SOUNDCARD_IQ_CENTRE", true},
    {FOXAPP_OP_BOOKMARK_IMPORT_FILE, "APP_BOOKMARK_IMPORT_FILE", true},
    {FOXAPP_OP_SCANNER_RANGE, "APP_SCANNER_RANGE", true},
    {FOXAPP_OP_USER_PRESET_APPLY, "APP_USER_PRESET_APPLY", true},
    {FOXAPP_OP_USER_PRESET_FORGET_AT, "APP_USER_PRESET_FORGET_AT", true},
    {FOXAPP_OP_PLUGIN_AUTO_PRESET, "APP_PLUGIN_AUTO_PRESET", true},
    {FOXAPP_OP_DECODER_STOP_LIST, "APP_DECODER_STOP_LIST", true},
    {FOXAPP_OP_STORE_UPDATE, "APP_STORE_UPDATE", true},
    {FOXAPP_OP_STORE_REMOVE_BLOCKED, "APP_STORE_REMOVE_BLOCKED", true},
};

constexpr std::size_t kOpCount = sizeof(kOps) / sizeof(kOps[0]);

struct KnownList {
    std::uint32_t ops[kOpCount];
    std::size_t n = 0;
    KnownList() {
        for (const OpEntry& e : kOps) {
            if (e.implemented) { ops[n++] = e.op; }
        }
    }
};

}  // namespace

const char* opName(std::uint32_t op) {
    for (const OpEntry& e : kOps) {
        if (e.op == op) { return e.name; }
    }
    return "?";
}

bool isKnownOp(std::uint32_t op) {
    for (const OpEntry& e : kOps) {
        if (e.op == op) { return e.implemented; }
    }
    return false;
}

const std::uint32_t* knownOps(std::size_t& count) {
    static const KnownList list;
    count = list.n;
    return list.ops;
}

}  // namespace cascade::core::cmd
