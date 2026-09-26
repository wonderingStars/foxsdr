// receiver_snapshot.cpp - see receiver_snapshot.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/receiver_snapshot.hpp"

#include <cstring>
#include <utility>

namespace cascade::core {

namespace {

bool changed(std::uint32_t a, std::uint32_t b, std::uint32_t mask) { return (a & mask) != (b & mask); }

bool sameText(const char* a, const char* b, std::size_t n) { return std::strncmp(a, b, n) == 0; }

// THE ENGINE API'S GROUPS (foxsdr_api.h, FoxReceiverState and docs/API.md
// 7.1): which fields move which counter. `seq` moves whenever anything but a
// measurement changes; the measurements are signalDb, sMeter, audioLevelDb,
// audioUnderruns and the two key timers, and - read as measurements here
// though API 0.2's list does not name it - pilotLevel, which follows the
// signal exactly as signalDb does (docs/engine-stage2.md, OPEN).
constexpr std::uint32_t kModeFlags =
    FOXAPI_RX_STEREO_ENABLED | FOXAPI_RX_NR | FOXAPI_RX_NOTCH | FOXAPI_RX_AUTO_NOTCH;
constexpr std::uint32_t kDeviceFlags = FOXAPI_RX_RUNNING | FOXAPI_RX_DEVICE_OPEN |
                                       FOXAPI_RX_FAULTED | FOXAPI_RX_DEVICE_AGC |
                                       FOXAPI_RX_AGC_SUPPORTED;
constexpr std::uint32_t kAudioFlags = FOXAPI_RX_MUTED | FOXAPI_RX_SINK_OPEN;
constexpr std::uint32_t kTxFlags = FOXAPI_RX_TX_AVAILABLE | FOXAPI_RX_TX_KEYED |
                                   FOXAPI_RX_TX_LATCHED | FOXAPI_RX_TX_KEY_MINE |
                                   FOXAPI_RX_TX_REMOTE_ARMED |
                                   FOXAPI_RX_TX_LATCH_RELEASE_FIRST;

bool gainTablesDiffer(const AppStateExt& f, const AppStateExt& o) {
    // Exactly the host API level 1's comparison before stage 2
    // (PluginApiCore::publish at bca426f): the count, then each published
    // stage whole, then the rate list.
    bool moved = f.abiGainCount != o.abiGainCount;
    for (std::uint32_t i = 0; !moved && i < f.abiGainCount && i < kMaxPublishedGains; ++i) {
        const PublishedGain& a = f.gains[i];
        const PublishedGain& b = o.gains[i];
        moved = std::strncmp(a.name, b.name, CASCADE_GAIN_NAME_CHARS) != 0 || a.unit != b.unit ||
                a.minDb != b.minDb || a.maxDb != b.maxDb || a.stepDb != b.stepDb ||
                a.currentDb != b.currentDb;
    }
    bool ratesMoved = f.rateCount != o.rateCount;
    for (std::uint32_t i = 0; !ratesMoved && i < f.rateCount && i < kMaxPublishedRates; ++i) {
        ratesMoved = f.rates[i] != o.rates[i];
    }
    return moved || ratesMoved;
}

}  // namespace

PublishedState initialPublishedState() {
    PublishedState s{};
    s.rx.signalDb = -200.0;
    s.rx.demodMode = FOXAPI_DEMOD_NFM;
    s.app.published = false;
    return s;
}

ReceiverSnapshot::ReceiverSnapshot() {
    const PublishedState initial = initialPublishedState();
    box_.store(initial);
    last_ = initial;
    full_ = std::make_shared<const Full>(Full{initial, nullptr});
}

void ReceiverSnapshot::publish(const PublishedState& facts,
                               std::shared_ptr<const net::RadioStatus> lists) {
    PublishedState w = facts;
    FoxReceiverState& f = w.rx;
    AppStateExt& e = w.app;
    const FoxReceiverState& o = last_.rx;
    const AppStateExt& oe = last_.app;
    const bool first = !havePublished_;

    // --- the engine API's counters (never below 1 once published) -----------
    const bool gainsMoved = gainTablesDiffer(e, oe);
    const bool tune = first || f.centreHz != o.centreHz || f.vfoOffsetHz != o.vfoOffsetHz;
    const bool mode = first || f.demodMode != o.demodMode || f.bandwidthHz != o.bandwidthHz ||
                      f.squelchDb != o.squelchDb || f.deemphasis != o.deemphasis ||
                      f.nrStrength != o.nrStrength || f.notchHz != o.notchHz ||
                      f.notchQ != o.notchQ || changed(f.flags, o.flags, kModeFlags);
    const bool device = first || changed(f.flags, o.flags, kDeviceFlags) ||
                        f.sampleRateHz != o.sampleRateHz || f.channelRateHz != o.channelRateHz ||
                        f.gainCount != o.gainCount ||
                        !sameText(f.deviceName, o.deviceName, FOXAPI_NAME_CHARS) ||
                        !sameText(f.faultMessage, o.faultMessage, FOXAPI_MESSAGE_CHARS) ||
                        gainsMoved;
    const bool audio = first || f.volume != o.volume || changed(f.flags, o.flags, kAudioFlags) ||
                       !sameText(f.sinkName, o.sinkName, FOXAPI_NAME_CHARS);
    const bool display = first || f.dbMin != o.dbMin || f.dbMax != o.dbMax;
    const bool tx = first || changed(f.flags, o.flags, kTxFlags) || f.txMode != o.txMode ||
                    f.txFrequencyHz != o.txFrequencyHz || f.txPowerDb != o.txPowerDb ||
                    !sameText(f.txUnkeyReason, o.txUnkeyReason, FOXAPI_MESSAGE_CHARS);
    // Every other flag (recording, scanner, decoder lamp, web listener, the
    // squelch and stereo lamps) and the decoder counts: the overall counter.
    const bool other = first || f.flags != o.flags || f.decodersRunning != o.decodersRunning ||
                       f.decodersFitted != o.decodersFitted;
    f.seq = o.seq + ((tune || mode || device || audio || display || tx || other) ? 1u : 0u);
    f.tuneSeq = o.tuneSeq + (tune ? 1u : 0u);
    f.modeSeq = o.modeSeq + (mode ? 1u : 0u);
    f.deviceSeq = o.deviceSeq + (device ? 1u : 0u);
    f.audioSeq = o.audioSeq + (audio ? 1u : 0u);
    f.displaySeq = o.displaySeq + (display ? 1u : 0u);
    f.txSeq = o.txSeq + (tx ? 1u : 0u);
    // No list is compared in stage 2 (docs/engine-stage2.md, OPEN): the
    // counter is 1 from the first publish, as "published, never changed".
    f.listSeq = first ? 1u : o.listSeq;
    f.grants = 0;  // per session; patched in on read once sessions exist
    f.structSize = static_cast<std::uint32_t>(sizeof(FoxReceiverState));

    // --- the host API level 1's counters: ITS groups, exactly as before -------
    //
    // plugin_abi.h: tuneSeq is centre and offset; modeSeq is mode, bandwidth
    // and squelch (NOT the audio DSP); deviceSeq is running, the radio, its
    // AGC, rate, name and gain/rate tables; audioSeq is volume and mute; and
    // the stereo lamp moves only seq. Different from the groups above, and
    // a plugin's counters must not change meaning under it, so they are
    // kept separately.
    {
        constexpr std::uint32_t kAbiDevice = FOXAPI_RX_RUNNING | FOXAPI_RX_DEVICE_OPEN |
                                             FOXAPI_RX_DEVICE_AGC | FOXAPI_RX_AGC_SUPPORTED;
        const bool aTune = tune;  // the same two fields
        const bool aMode = first || f.demodMode != o.demodMode || f.bandwidthHz != o.bandwidthHz ||
                           f.squelchDb != o.squelchDb;
        const bool aDevice = first || changed(f.flags, o.flags, kAbiDevice) ||
                             f.sampleRateHz != o.sampleRateHz ||
                             !sameText(f.deviceName, o.deviceName, CASCADE_DEVICE_NAME_CHARS) ||
                             gainsMoved;
        const bool aAudio = first || f.volume != o.volume || changed(f.flags, o.flags, FOXAPI_RX_MUTED);
        const bool aOther = first || changed(f.flags, o.flags, FOXAPI_RX_STEREO_ACTIVE);
        e.abiTuneSeq = oe.abiTuneSeq + (aTune ? 1u : 0u);
        e.abiModeSeq = oe.abiModeSeq + (aMode ? 1u : 0u);
        e.abiDeviceSeq = oe.abiDeviceSeq + (aDevice ? 1u : 0u);
        e.abiAudioSeq = oe.abiAudioSeq + (aAudio ? 1u : 0u);
        e.abiSeq = oe.abiSeq + ((aTune || aMode || aDevice || aAudio || aOther) ? 1u : 0u);
    }
    e.published = true;

    last_ = w;
    havePublished_ = true;
    box_.store(w);

    // --- the whole block, for the readers that need the lists too -------------
    std::shared_ptr<const Full> block = std::make_shared<const Full>(Full{w, std::move(lists)});
    std::unique_lock<std::mutex> lk(fullMutex_, std::try_to_lock);
    if (lk.owns_lock()) {
        full_.swap(block);  // the previous block is released below, unlocked
        lk.unlock();
    } else {
        deferred_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ReceiverSnapshot::read(PublishedState& out) const { return box_.load(out); }

std::shared_ptr<const ReceiverSnapshot::Full> ReceiverSnapshot::readFull() const {
    std::lock_guard<std::mutex> lk(fullMutex_);
    return full_;
}

}  // namespace cascade::core
