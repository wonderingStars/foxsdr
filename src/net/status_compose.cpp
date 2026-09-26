// status_compose.cpp - see status_compose.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/status_compose.hpp"

namespace cascade::net {

const char* demodName(std::uint32_t foxDemod) {
    return foxDemod >= 1u && foxDemod <= 8u ? kDemodNames[foxDemod] : "";
}

const char* scannerStateName(std::uint32_t state) {
    switch (state) {
        case cascade::core::kScannerIdle: return "idle";
        case cascade::core::kScannerScanning: return "scanning";
        case cascade::core::kScannerPaused: return "paused";
        case cascade::core::kScannerHolding: return "holding";
        default: return "";
    }
}

RadioStatus composeRadioStatus(const cascade::core::PublishedState& state, const RadioStatus* lists) {
    if (!state.app.published) { return RadioStatus{}; }
    RadioStatus o = lists != nullptr ? *lists : RadioStatus{};
    const FoxReceiverState& r = state.rx;
    const cascade::core::AppStateExt& e = state.app;
    const auto flag = [&r](std::uint32_t f) { return (r.flags & f) != 0u; };

    // Every scalar member of RadioStatus, in its declaration order. The
    // strings and lists (faultMessage, sourceName, tunerDisplayStyle, rdsPs,
    // rdsRadioText, sourceKind, soapyArgs, antenna, antennas, devices, gains,
    // sourceError, audioMutedBy, audioSource, recordDir, recordError,
    // recordNotice, bookmarks, decoded, tracks, plugins, catalogue,
    // catalogueStatus, catalogueError, installReport, installError,
    // basemap.attribution, images) are `lists`' own.
    o.running = flag(FOXAPI_RX_RUNNING);
    o.faulted = flag(FOXAPI_RX_FAULTED);
    o.centerHz = r.centreHz;
    o.sampleRateHz = r.sampleRateHz;
    o.vfoOffsetHz = r.vfoOffsetHz;
    o.bandwidthHz = r.bandwidthHz;
    o.mode = demodName(r.demodMode);
    o.signalDb = static_cast<float>(r.signalDb);
    o.stereoActive = flag(FOXAPI_RX_STEREO_ACTIVE);
    o.transmitting = flag(FOXAPI_RX_TX_KEYED);
    // The app's remote key needs a transmitter open AND the Transmit page on
    // screen: that pair IS its consent to remote PTT (docs/engine-stage2.md).
    o.transmitAvailable = flag(FOXAPI_RX_TX_REMOTE_ARMED);
    o.transmitRemoteHoldMs = r.txHoldRemainingMs;
    o.squelchDb = static_cast<float>(r.squelchDb);
    o.volume = static_cast<float>(r.volume);
    o.dbMin = static_cast<float>(r.dbMin);
    o.dbMax = static_cast<float>(r.dbMax);
    o.deemphasisIndex = static_cast<int>(r.deemphasis);
    o.nrEnabled = flag(FOXAPI_RX_NR);
    o.nrStrength = static_cast<float>(r.nrStrength);
    o.notchEnabled = flag(FOXAPI_RX_NOTCH);
    o.notchFreqHz = r.notchHz;
    o.notchQ = r.notchQ;
    o.autoNotch = flag(FOXAPI_RX_AUTO_NOTCH);
    o.autoNotchEngaged = e.autoNotchEngaged;
    o.autoNotchFreqHz = e.autoNotchFreqHz;
    o.stereoEnabled = flag(FOXAPI_RX_STEREO_ENABLED);
    o.pilotLocked = e.pilotLocked;
    o.rdsSynced = e.rdsSynced;
    o.rdsPiValid = e.rdsPiValid;
    o.rdsPi = e.rdsPi;
    o.rdsPsValid = e.rdsPsValid;
    o.rdsPty = e.rdsPty;
    o.rdsTp = e.rdsTp;
    o.rdsTa = e.rdsTa;
    o.rdsGroups = e.rdsGroups;
    o.rdsErrors = e.rdsErrors;
    o.agcSupported = flag(FOXAPI_RX_AGC_SUPPORTED);
    o.agc = flag(FOXAPI_RX_DEVICE_AGC);
    o.sourceBusy = e.sourceBusy;
    o.iqRecording = flag(FOXAPI_RX_RECORDING_IQ);
    o.audioUnderruns = r.audioUnderruns;
    o.audioPrimingCallbacks = e.audioPrimingCallbacks;
    o.audioRingMs = e.audioRingMs;
    o.audioRingCapacityMs = e.audioRingCapacityMs;
    o.audioPluginGaps = e.audioPluginGaps;
    o.audioPluginGapFrames = e.audioPluginGapFrames;
    o.audioRecording = flag(FOXAPI_RX_RECORDING_AUDIO);
    o.iqBytes = e.iqBytes;
    o.audioBytes = e.audioBytes;
    o.scannerActive = flag(FOXAPI_RX_SCANNER_ACTIVE);
    o.scannerState = scannerStateName(e.scannerState);
    o.scanStartHz = e.scanStartHz;
    o.scanStopHz = e.scanStopHz;
    o.scanStepHz = e.scanStepHz;
    o.rxPositionSet = e.rxPositionSet;
    o.rxLatDeg = e.rxLatDeg;
    o.rxLonDeg = e.rxLonDeg;
    o.catalogueBusy = e.catalogueBusy;
    o.basemap.active = e.basemapActive;
    o.basemap.minZoom = e.basemapMinZoom;
    o.basemap.maxZoom = e.basemapMaxZoom;
    o.basemap.tileSize = e.basemapTileSize;
    return o;
}

}  // namespace cascade::net
