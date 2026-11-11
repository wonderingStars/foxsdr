// engine_radios.cpp - radios 2 to 8 (multi-radio engine, stage 4; docs/multi-radio-records/engine-stage-4.md).
//
// Stage 1 made the receiver a real object (Receiver) with the engine holding eight slots; stage 3 made a command and a read name a radio. This file makes
// the other seven slots LIVE: RADIO_ADD builds a Receiver (its own pipeline with its own source and DSP threads, device, fault and retry state, health
// counters, recorders and published snapshot), RADIO_REMOVE stops it and lets it go, and the pass runs each added radio's own housekeeping and publish
// beside the first radio's, whose functions are unchanged.
//
// HOW AN ENGINE FUNCTION ACTS ON A RADIO: through rx() (engine.hpp) - the slot the calling thread selected with a RadioWork/SlotScope, or slot 0 when
// none. applyCommand selects the slot a command is addressed to; the per-radio functions below select each added radio for its own work. Everything that
// is ENGINE-WIDE (the band plan, bookmarks, markers, the scanner and airband monitor, the transmitter, the plugin machinery, the servers, the config) is
// still the engine's: see the record for each op and for what is left slot-0-biased on purpose.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/diag_log.hpp"
#include "core/patch_devices.hpp"
#include "core/receiver_tables.hpp"
#include "source/siggen_source.hpp"

namespace pc = cascade::core::patch;
using namespace cascade::gui;   // kSampleRateHz, kFftSize, kAveragingAlpha (core/receiver_tables.hpp), as engine.cpp does

namespace cascade::engine {

namespace {

const char* routeWord(int route) {
    switch (route) {
        case cascade::sink::AudioOut::kRouteLeft: return "left";
        case cascade::sink::AudioOut::kRouteRight: return "right";
        case cascade::sink::AudioOut::kRouteNone: return "none";
        default: return "both";
    }
}

}  // namespace

const char* radioRouteWord(int route) { return routeWord(route); }

std::shared_ptr<cascade::core::ReceiverSnapshot> Engine::snapshotOf(std::uint32_t radio) const {
    if (radio < 1u || radio > kMaxReceivers) { return nullptr; }
    std::shared_lock<std::shared_mutex> lk(receiverSlotsMutex_);
    const std::unique_ptr<Receiver>& r = receivers_[radio - 1u];
    return r ? r->receiverSnapshot_ : nullptr;
}

std::string Engine::takePrefix(bool iq) {
    const std::uint32_t n = static_cast<std::uint32_t>(scopedSlot()) + 1u;
    // The first radio's files keep the names they have always had while it is the only radio; as soon as there are several, EVERY radio's say which.
    if (n == 1u && receiverCount() <= 1u) { return std::string(); }
    return "radio" + std::to_string(n) + (iq ? "_iq" : "_audio");
}

// --- RADIO_ADD ---------------------------------------------------------------------------------------------------------------------------------
//
//   ival[0]  the radio's number, 2..kMaxReceivers; 0 = the lowest free one
//   ival[1]  1 = start it running as soon as it is open; 0 = leave it stopped
//   text     the device: "" or "generator" is a TEST RADIO on the built-in signal generator; anything else is a device id as FOXAPI_LIST_DEVICES lists it
//   num[0]   a test radio's tone, Hz from its centre (0 = 25 kHz times the radio's number); only for the generator
//
// Refused, never half-done: a number that is not 2..N is OUT_OF_RANGE; a radio that is there already answers NO_CHANGE; a device another radio holds is
// BUSY with "in use by Radio N"; the limit on open radios (receivers and patch radios together) is LIMIT, in the ledger's words, which name every holder.
FoxCommandResult Engine::radioAddOp(const FoxCommand& c, const std::string& text, FoxCommandResult res) {
    const auto refuse = [&res](std::int32_t status, const std::string& why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why.c_str());
        return res;
    };
    std::size_t slot = kMaxReceivers;
    if (c.ival[0] != 0) {
        if (c.ival[0] < 2 || c.ival[0] > static_cast<long long>(kMaxReceivers)) {
            return refuse(FOXAPI_OUT_OF_RANGE, "radio numbers run from 2 to " + std::to_string(kMaxReceivers) + "; radio 1 is always there");
        }
        slot = static_cast<std::size_t>(c.ival[0] - 1);
        if (receivers_[slot]) {
            res.status = FOXAPI_NO_CHANGE;
            res.applied[0] = static_cast<double>(slot + 1u);
            std::snprintf(res.message, sizeof(res.message), "that radio is open already");
            return res;
        }
    } else {
        for (std::size_t s = 1; s < kMaxReceivers; ++s) {
            if (!receivers_[s]) {
                slot = s;
                break;
            }
        }
    }
    // THE LEDGER, FIRST: it knows every open radio (receivers and patch radios) and who holds which device, and it is the one place that says no.
    rebuildDeviceLedger();
    if (slot >= kMaxReceivers) { return refuse(FOXAPI_LIMIT, deviceLedger_.limitSentence()); }
    const bool generator = text.empty() || text == "generator";
    std::string key;
    if (!generator) {
        const std::size_t colon = text.find(':');
        // A device id is "<kind>:<args>"; the panels' own ("file:", "open-pluto:", "row:", "soundcard:") are not devices of a scanned list.
        if (colon == std::string::npos) { return refuse(FOXAPI_NOT_FOUND, "no scanned device matches that id"); }
        const std::string kind = text.substr(0, colon);
        if (kind == "file" || kind == "open-pluto" || kind == "open-rtltcp" || kind == "row" || kind == "soundcard") {
            return refuse(FOXAPI_UNSUPPORTED, "a second radio opens a scanned device or the signal generator");
        }
        key = pc::makeDeviceKey(kind, text.substr(colon + 1));
    }
    const DeviceLedger::Claim claim = deviceLedger_.claim(key, DeviceHolder::receiver(slot), true);
    if (!claim.granted()) {
        return refuse(claim.verdict == DeviceLedger::Verdict::LimitReached ? FOXAPI_LIMIT : FOXAPI_BUSY, claim.refusal);
    }

    // THE SLOT COMES INTO BEING: the receiver is built whole outside the lock (its pipeline allocates), then put in under the exclusive lock and the mask
    // bit raised. Nothing reads a slot whose bit is not set.
    std::unique_ptr<Receiver> r = std::make_unique<Receiver>(cascade::core::Pipeline::Config{kSampleRateHz, kFftSize, kAveragingAlpha, /*audioEnabled=*/true});
    r->radioNumber_ = static_cast<std::uint32_t>(slot) + 1u;
    // THE PLUGIN API CORE OWNS EVERY RADIO'S SNAPSHOT (stage 6), so a plugin's get_state on this radio never meets a snapshot being freed: the receiver adopts the core's, and
    // it is emptied (a default state is published) so that a radio added where another was reads as new until its first pass.
    r->receiverSnapshot_ = pluginApi_->radioSnapshot(static_cast<std::uint32_t>(slot) + 1u);
    r->receiverSnapshot_->publish(cascade::core::PublishedState{}, nullptr, std::vector<std::uint64_t>());
    r->claimedKey_ = key;
    {
        std::unique_lock<std::shared_mutex> lk(receiverSlotsMutex_);
        receivers_[slot] = std::move(r);
    }
    receiverMask_.fetch_or(1u << slot, std::memory_order_acq_rel);
    cascade::core::diagLogf("radio: radio %u added (%s)", static_cast<unsigned>(slot + 1u), generator ? "test radio on the signal generator" : "a scanned device");
    {
        RadioWork w(*this, slot);
        // THE TEST RADIO'S TONE: one carrier at an offset that tells the radios apart, over a quiet floor. Each radio has its own generator (it is inside
        // its own pipeline), so two radios can never hear each other's tone.
        if (generator) {
            const double offset = c.num[0] != 0.0 ? c.num[0] : 25000.0 * static_cast<double>(slot + 1u);
            rx().testToneOffsetHz_ = offset;
            cascade::source::SigGen& gen = rx().pipeline_.sigGen();
            gen.setTone(0, offset, -20.0f);
            gen.setNoiseFloorDb(-90.0f);
        }
        // THE SAME OUTPUT AS THE FIRST RADIO'S, when that one has an open sink: every radio plays through its own stream to the chosen device and the
        // system mixes them (design section 6).
        // (A Pipeline opens the system default output when it is built, as the first radio's did: a radio that is already playing on the output the first
        // radio chose is left alone - reopening a stream that is open disturbs it - and one that is not is opened on it.)
        cascade::sink::AudioOut& first = receivers_[0]->pipeline_.audio();
        cascade::sink::AudioOut& mine = rx().pipeline_.audio();
        if (first.everOpened() && (!mine.everOpened() || !mine.streamAlive() || mine.openedDeviceRequested() != first.openedDeviceRequested())) {
            (void)requestAudioOpen(first.openedDeviceRequested(), false);
        }
        if (!generator) {
            rx().sourceError_.clear();
            if (restoreEntry_ != nullptr) {
                // A RESTORE (engine stage 5): the start-up open, by the saved arguments, with the saved rate, antenna and centre - not a pick from the scanned lists.
                restoreExtraRadioDevice(text.substr(0, text.find(':')), text.substr(text.find(':') + 1), restoreEntry_->sampleRateHz, restoreEntry_->antenna,
                                        restoreEntry_->centerHz);
            } else if (!selectSourceById(text)) {
                // The radio stays (it is on the generator, like the first radio after a failed open); the words say why.
                rx().refusal_ = rx().sourceError_;
            }
        }
        // ITS RUNNER (stage 6): the decoders that run on this radio, if any (none until the user puts one here), against its own source.
        createRadioRunner(slot);
        if (c.ival[1] != 0) { startReceiver(); }
    }
    applyAudioRoutes();
    res.applied[0] = static_cast<double>(slot + 1u);
    return res;
}

// --- RADIO_REMOVE ------------------------------------------------------------------------------------------------------------------------------
//
//   ival[0]  the radio's number, 2..kMaxReceivers (radio 1 is always there)
FoxCommandResult Engine::radioRemoveOp(const FoxCommand& c, FoxCommandResult res) {
    const auto refuse = [&res](std::int32_t status, const char* why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why);
        return res;
    };
    if (c.ival[0] < 1 || c.ival[0] > static_cast<long long>(kMaxReceivers)) { return refuse(FOXAPI_OUT_OF_RANGE, "no such radio number"); }
    if (c.ival[0] == 1) { return refuse(FOXAPI_UNSUPPORTED, "radio 1 is always there"); }
    const std::size_t slot = static_cast<std::size_t>(c.ival[0] - 1);
    if (!receivers_[slot]) { return refuse(FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    removeReceiverSlot(slot);
    res.applied[0] = static_cast<double>(slot + 1u);
    return res;
}

void Engine::removeReceiverSlot(std::size_t slot) {
    if (slot == 0 || slot >= kMaxReceivers || !receivers_[slot]) { return; }
    cascade::core::diagLogf("radio: radio %u removed", static_cast<unsigned>(slot + 1u));
    {
        RadioWork w(*this, slot);
        rx().removing_ = true;
        // THE PLUGINS THAT RAN HERE GO FIRST (stage 6): back to radio 1, stopped, their instances destroyed and the runner detached - before the radio's threads end and before
        // its snapshot, which a plugin's get_state reads, goes quiet.
        destroyRadioRunner(slot);
        // THE TAKES END FIRST and the threads are joined by the stop, as a user's Stop does (stopReceiver): a take can never outlive the sample flow it
        // taps. Then an open still in flight is reaped (a detached worker releases the device if it lands after), and the radio lets go of its device.
        // THE STOP IS THE ONE STOP: RUN 0 aimed at this radio, through applyCommand (stopReceiver is applyCommand's alone - tests/test_stop_ends_recordings
        // holds every caller to it), which ends the takes first and joins the threads.
        FoxCommand stop = cascade::core::cmd::makeInt(FOXAPI_OP_RUN, 0);
        stop.target = static_cast<std::uint32_t>(slot) + 1u;
        (void)applyCommand(stop);
        endTakes(true, true, "the radio was removed", true);
        reapPendingDeviceOpen();
        rx().device_ = nullptr;
        rx().soapyView_ = nullptr;
        rx().pipeline_.audio().close();
    }
    if (scannerSlot_ == slot && scanner_.active()) { scanner_.stop(); }
    if (scannerSlot_ == slot) { scannerSlot_ = 0; }
    if (soloRadio_ == slot + 1u) { soloRadio_ = 0; }
    // NOTHING READS THE SLOT FROM NOW ON: the bit goes first, then the receiver is taken out under the exclusive lock (a reader that took the shared lock
    // before it has finished with it), and destroyed outside the lock - its pipeline joins its threads and its source closes the device.
    receiverMask_.fetch_and(~(1u << slot), std::memory_order_acq_rel);
    std::unique_ptr<Receiver> dead;
    {
        std::unique_lock<std::shared_mutex> lk(receiverSlotsMutex_);
        dead = std::move(receivers_[slot]);
    }
    dead.reset();
    api_->radioClosed(slot);
    rebuildDeviceLedger();
    applyAudioRoutes();
}

// --- RADIO_AUDIO and RADIO_SOLO ----------------------------------------------------------------------------------------------------------------
//
// RADIO_AUDIO  ival[0] the radio (0 = the sender's focus), ival[1] the route: 0 both, 1 left, 2 right, 3 none (AudioOut::kRoute*).
// RADIO_SOLO   ival[0] the radio that is heard alone; 0 = nobody is solo (every radio plays by its own route and mute).
//
// The route is the user's choice per radio and survives a solo. The solo is held in each OTHER radio's sink as a silence that ramps in and out
// (AudioOut::setSilenced), so it never touches a mute the user set: un-soloing gives every radio back exactly what it had.
FoxCommandResult Engine::radioAudioOp(const FoxCommand& c, FoxCommandResult res) {
    const auto refuse = [&res](std::int32_t status, const char* why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why);
        return res;
    };
    std::uint32_t radio = static_cast<std::uint32_t>(std::max<long long>(0, std::min<long long>(c.ival[0], 0xFFFF)));
    if (radio == 0u) { radio = commandFocus_; }
    if (!receiverLive(radio)) { return refuse(FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    if (c.ival[1] < 0 || c.ival[1] > cascade::sink::AudioOut::kRouteNone) { return refuse(FOXAPI_OUT_OF_RANGE, "no such route"); }
    Receiver& r = *receivers_[radio - 1u];
    res.applied[0] = static_cast<double>(c.ival[1]);
    if (r.audioRoute_ == static_cast<int>(c.ival[1])) {
        res.status = FOXAPI_NO_CHANGE;
        return res;
    }
    r.audioRoute_ = static_cast<int>(c.ival[1]);
    applyAudioRoutes();
    return res;
}

FoxCommandResult Engine::radioSoloOp(const FoxCommand& c, FoxCommandResult res) {
    const auto refuse = [&res](std::int32_t status, const char* why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why);
        return res;
    };
    if (c.ival[0] < 0 || c.ival[0] > static_cast<long long>(kMaxReceivers)) { return refuse(FOXAPI_OUT_OF_RANGE, "no such radio number"); }
    const std::uint32_t radio = static_cast<std::uint32_t>(c.ival[0]);
    if (radio != 0u && !receiverLive(radio)) { return refuse(FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    res.applied[0] = static_cast<double>(radio);
    if (radio == soloRadio_) {
        res.status = FOXAPI_NO_CHANGE;
        return res;
    }
    soloRadio_ = radio;
    applyAudioRoutes();
    return res;
}

void Engine::applyAudioRoutes() {
    for (std::size_t slot = 0; slot < kMaxReceivers; ++slot) {
        const std::unique_ptr<Receiver>& r = receivers_[slot];
        if (!r) { continue; }
        cascade::sink::AudioOut& out = r->pipeline_.audio();
        out.setRoute(r->audioRoute_);
        out.setSilenced(soloRadio_ != 0u && soloRadio_ != slot + 1u);
    }
}

// --- the added radios' own pass -----------------------------------------------------------------------------------------------------------------
//
// What the first radio's pass does for it in pumpWorkers / pumpAudio / pumpInputs, run for each added radio under its own scope. Only the functions that are
// one radio's: the scan, the plugin store, the catalogue and the other engine-wide polls stay as they were (once a pass, not once a radio).
void Engine::pumpExtraReceivers() {
    telemetryNoteRadios();
    for (std::size_t slot = 1; slot < kMaxReceivers; ++slot) {
        if (!receivers_[slot]) { continue; }
        RadioWork w(*this, slot);
        const double nowS = clockS();
        pollRecordStarts(nowS);
        pollDeviceOpen();
        pollSoapyRecovery();
        pollPendingRetune();
        endTakesOnFault();
        pollAudioOpen();
        pollAudioHealth();
        pollDspMinute();
        pollSavedRadioRetry();   // (a saved device that was only busy is tried again, as the first radio's is: engine stage 5)
        // A radio whose open failed holds nothing: the ledger is told by the key going (the radio sits on the generator, as the first radio does).
        if (rx().device_ == nullptr && !rx().deviceOpenPending_) { rx().claimedKey_.clear(); }
    }
}

void Engine::publishExtraReceivers() {
    for (std::size_t slot = 1; slot < kMaxReceivers; ++slot) {
        if (!receivers_[slot]) { continue; }
        RadioWork w(*this, slot);
        const std::string faultMessage = rx().pipeline_.faultMessage();
        const cascade::core::RdsSnapshot rds = rx().pipeline_.rdsSnapshot();
        cascade::core::PublishedState ps{};
        fillPublishedState(ps, faultMessage, rds);
        rx().apiFaultText_ = faultMessage;
        // The lists are the engine's, one set: every radio's state carries the fingerprint the first radio's publish took.
        ps.app.listsFingerprint = lastListsFingerprint_;
        std::shared_ptr<cascade::net::RadioStatus> lists = std::make_shared<cascade::net::RadioStatus>();
        lists->faultMessage = faultMessage;
        lists->sourceName = rx().pipeline_.activeSource().name();
        lists->sourceKind = rx().sourceKind_;
        lists->soapyArgs = rx().deviceArgs_;
        lists->sourceError = rx().sourceError_;
        rx().receiverSnapshot_->publish(ps, std::move(lists), std::vector<std::uint64_t>());
    }
}

}  // namespace cascade::engine
