// control_ops.cpp - see control_ops.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/control_ops.hpp"

#include <cstring>
#include <string>

namespace cascade::net {

namespace cmd = cascade::core::cmd;

namespace {

void push(std::vector<cmd::QueuedCommand>& out, const FoxCommand& c) {
    cmd::QueuedCommand q;
    q.c = c;
    out.push_back(std::move(q));
}

}  // namespace

std::vector<cmd::QueuedCommand> controlRequestToCommands(const ControlRequest& r,
                                                         const ControlOpsContext& ctx) {
    std::vector<cmd::QueuedCommand> out;

    // --- the receiver, in applyControlRequest's historical order ------------
    if (r.running) { push(out, cmd::makeInt(FOXAPI_OP_RUN, *r.running ? 1 : 0)); }
    if (r.centerHz) { push(out, cmd::makeNum(FOXAPI_OP_SET_CENTRE, *r.centerHz)); }
    if (r.mode) {
        // By NAME, never by index: DemodMode's order and the mode keys' order
        // differ. A name the keys do not carry becomes demod 0, which
        // applyCommand refuses and which changes nothing - as the old loop
        // that found no match changed nothing.
        push(out, cmd::makeInt(FOXAPI_OP_SET_MODE,
                               cmd::foxDemodFromName(cascade::dsp::modeName(*r.mode))));
    }
    // Bandwidth before offset: the offset's limit depends on it.
    if (r.bandwidthHz) { push(out, cmd::makeNum(FOXAPI_OP_SET_BANDWIDTH, *r.bandwidthHz)); }
    if (r.vfoOffsetHz) { push(out, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, *r.vfoOffsetHz)); }
    if (r.squelchDb) { push(out, cmd::makeNum(FOXAPI_OP_SET_SQUELCH, *r.squelchDb)); }
    if (r.volume) { push(out, cmd::makeNum(FOXAPI_OP_SET_VOLUME, *r.volume)); }
    // The display range: the end the caller moved yields to the minimum span.
    if (r.dbMin && r.dbMax) {
        push(out, cmd::makeNum(FOXAPI_OP_SET_DISPLAY_RANGE, *r.dbMin, *r.dbMax));
    } else if (r.dbMin) {
        push(out, cmd::makeNum(FOXAPP_OP_SET_DISPLAY_MIN, *r.dbMin));
    } else if (r.dbMax) {
        push(out, cmd::makeNum(FOXAPP_OP_SET_DISPLAY_MAX, *r.dbMax));
    }
    if (r.deemphasisIndex) { push(out, cmd::makeInt(FOXAPI_OP_SET_DEEMPHASIS, *r.deemphasisIndex)); }
    if (r.stereoEnabled) { push(out, cmd::makeInt(FOXAPI_OP_SET_STEREO, *r.stereoEnabled ? 1 : 0)); }
    // Noise reduction: the switch, then the strength, as they always were -
    // one SET_NR when both came, the strength alone when only it did.
    if (r.nrEnabled) {
        FoxCommand c = cmd::makeInt(FOXAPI_OP_SET_NR, *r.nrEnabled ? 1 : 0, r.nrStrength ? 1 : 0);
        if (r.nrStrength) { c.num[0] = *r.nrStrength; }
        push(out, c);
    } else if (r.nrStrength) {
        push(out, cmd::makeNum(FOXAPP_OP_SET_NR_STRENGTH, *r.nrStrength));
    }
    // The notch: SET_NOTCH carries the switch and, when both came, the
    // frequency and Q; a lone frequency or Q goes on its own.
    {
        bool freqDone = false;
        bool qDone = false;
        if (r.notchEnabled) {
            const bool both = r.notchFreqHz.has_value() && r.notchQ.has_value();
            FoxCommand c = cmd::makeInt(FOXAPI_OP_SET_NOTCH, *r.notchEnabled ? 1 : 0, both ? 1 : 0);
            if (both) {
                c.num[0] = *r.notchFreqHz;
                c.num[1] = *r.notchQ;
                freqDone = qDone = true;
            }
            push(out, c);
        }
        if (r.notchFreqHz && !freqDone) {
            push(out, cmd::makeNum(FOXAPP_OP_SET_NOTCH_FREQUENCY, *r.notchFreqHz));
        }
        if (r.notchQ && !qDone) { push(out, cmd::makeNum(FOXAPP_OP_SET_NOTCH_Q, *r.notchQ)); }
    }
    if (r.autoNotch) { push(out, cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, *r.autoNotch ? 1 : 0)); }

    // --- the source ------------------------------------------------------------
    if (r.scanDevices.value_or(false)) { push(out, cmd::make(FOXAPI_OP_SCAN_DEVICES)); }
    if (r.sourceKind) {
        // "siggen", or "<kind>:<args>" for a scanned device - the id
        // SELECT_SOURCE matches against the enumerated lists (never handed to
        // a driver verbatim). A device kind with no args names nothing and,
        // as before, does nothing.
        if (*r.sourceKind == "siggen") {
            out.push_back(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "siggen"));
        } else if (r.soapyArgs) {
            out.push_back(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, *r.sourceKind + ":" + *r.soapyArgs));
        }
    }
    if (r.antenna) { out.push_back(cmd::makeText(FOXAPI_OP_SET_ANTENNA, *r.antenna)); }
    if (r.sampleRateHz) { push(out, cmd::makeNum(FOXAPI_OP_SET_SAMPLE_RATE, *r.sampleRateHz)); }
    if (r.gainName && r.gainDb) {
        out.push_back(cmd::makeText(FOXAPI_OP_SET_GAIN, *r.gainName, 0, 0, *r.gainDb));
    }
    if (r.agc) { push(out, cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, *r.agc ? 1 : 0)); }

    // --- the recorder -----------------------------------------------------------
    if (r.recordIq) { push(out, cmd::makeInt(FOXAPI_OP_RECORD_IQ, *r.recordIq ? 1 : 0)); }
    if (r.recordAudio) { push(out, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, *r.recordAudio ? 1 : 0)); }

    // --- bookmarks: rows of the published list, named by id ----------------------
    const auto idForRow = [&ctx](int row) -> std::int64_t {
        if (row < 0 || static_cast<std::size_t>(row) >= ctx.bookmarkIdByRow.size()) { return 0; }
        return static_cast<std::int64_t>(ctx.bookmarkIdByRow[static_cast<std::size_t>(row)]);
    };
    if (r.bookmarkAdd) { out.push_back(cmd::makeText(FOXAPI_OP_BOOKMARK_ADD, *r.bookmarkAdd)); }
    if (r.bookmarkTune) { push(out, cmd::makeInt(FOXAPI_OP_BOOKMARK_TUNE, idForRow(*r.bookmarkTune))); }
    if (r.bookmarkRemove) {
        push(out, cmd::makeInt(FOXAPI_OP_BOOKMARK_REMOVE, idForRow(*r.bookmarkRemove)));
    }

    // --- the scanner's range (stored; a running scan is not reconfigured) -----
    double scanStart = ctx.scanStartHz;
    double scanStop = ctx.scanStopHz;
    double scanStep = ctx.scanStepHz;
    if (r.scanStartHz || r.scanStopHz || r.scanStepHz) {
        FoxCommand c = cmd::make(FOXAPP_OP_SCANNER_RANGE);
        std::int64_t mask = 0;
        if (r.scanStartHz) { mask |= 1; c.num[0] = scanStart = *r.scanStartHz; }
        if (r.scanStopHz) { mask |= 2; c.num[1] = scanStop = *r.scanStopHz; }
        if (r.scanStepHz) { mask |= 4; c.num[2] = scanStep = *r.scanStepHz; }
        c.ival[0] = mask;
        push(out, c);
    }

    // --- plugins and the store ------------------------------------------------------
    if (r.pluginFetch.value_or(false)) { push(out, cmd::make(FOXAPI_OP_STORE_FETCH)); }
    if (r.pluginInstall) {
        out.push_back(cmd::makeText(FOXAPI_OP_STORE_INSTALL, *r.pluginInstall,
                                    r.acknowledgeNotice.value_or(false) ? 1 : 0));
    }
    if (r.pluginRemove) { out.push_back(cmd::makeText(FOXAPI_OP_STORE_REMOVE, *r.pluginRemove)); }
    if (r.pluginTuneName && r.pluginTuneAllowed) {
        // ival[0] 1: the receiver-control (TUNE) grant.
        out.push_back(cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, *r.pluginTuneName, 1,
                                    *r.pluginTuneAllowed ? 1 : 0));
    }
    if (r.pluginPresetName && r.pluginPresetIndex) {
        out.push_back(cmd::makeText(FOXAPI_OP_PLUGIN_PRESET, *r.pluginPresetName, *r.pluginPresetIndex));
    }

    // --- the scanner's run state -------------------------------------------------------
    if (r.scannerActive) {
        FoxCommand c = cmd::makeInt(FOXAPI_OP_SCANNER_RUN, *r.scannerActive ? 1 : 0);
        if (*r.scannerActive) {
            c.num[0] = scanStart;
            c.num[1] = scanStop;
            c.num[2] = scanStep;
        }
        push(out, c);
    }
    if (r.scannerSkip.value_or(false)) { push(out, cmd::make(FOXAPI_OP_SCANNER_SKIP)); }

    // --- the remote transmit key, LAST ----------------------------------------------
    if (r.transmitPtt) { push(out, cmd::makeInt(FOXAPI_OP_TX_PTT, *r.transmitPtt ? 1 : 0)); }
    return out;
}

bool pluginControlToCommand(const cascade::core::PluginControl& c, FoxCommand& out) {
    using K = cascade::core::PluginControl::Kind;
    switch (c.kind) {
        // The frequency READOUT's own path: the VFO offset kept, the centre
        // moving - SET_FREQUENCY, never SET_CENTRE.
        case K::Frequency: out = cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, c.value); return true;
        case K::VfoOffset: out = cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, c.value); return true;
        // CASCADE_DEMOD_* and FOXAPI_DEMOD_* are the same numbers by design;
        // range-checked when it was queued, and again by applyCommand.
        case K::Mode: out = cmd::makeInt(FOXAPI_OP_SET_MODE, c.mode); return true;
        case K::Bandwidth: out = cmd::makeNum(FOXAPI_OP_SET_BANDWIDTH, c.value); return true;
        case K::Squelch: out = cmd::makeNum(FOXAPI_OP_SET_SQUELCH, c.value); return true;
        case K::SampleRate: out = cmd::makeNum(FOXAPI_OP_SET_SAMPLE_RATE, c.value); return true;
        case K::Gain: {
            out = cmd::makeNum(FOXAPI_OP_SET_GAIN, c.value);
            const void* nul = std::memchr(c.gainName, '\0', sizeof(c.gainName));
            const std::size_t len = (nul != nullptr)
                                        ? static_cast<std::size_t>(static_cast<const char*>(nul) - c.gainName)
                                        : sizeof(c.gainName);
            const std::string name(c.gainName, len);
            (void)cmd::putText(out, name);  // a stage name always fits 255 bytes
            return true;
        }
        case K::DeviceAgc: out = cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, c.flag ? 1 : 0); return true;
        case K::Running: out = cmd::makeInt(FOXAPI_OP_RUN, c.flag ? 1 : 0); return true;
        case K::Volume: out = cmd::makeNum(FOXAPI_OP_SET_VOLUME, c.value); return true;
        case K::Muted: out = cmd::makeInt(FOXAPI_OP_SET_MUTED, c.flag ? 1 : 0); return true;
    }
    return false;
}

}  // namespace cascade::net
