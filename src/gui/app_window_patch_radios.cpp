// app_window_patch_radios.cpp - the patch page's own radios and speaker
// outputs (0.99.17). AppWindow members, kept out of app_window.cpp because
// they are one subject: which devices the patch has open, where each
// speaker's sound goes, and handing the receiver's radio over and back.
//
// THE OWNER'S FOUR RULES, each of which is a function below:
//   - "add up to 5 sdrs ... each sdr input and out set in the patch panel":
//     every Radio node names its own device, rate and centre, and runs its
//     own core::patch::PatchRadio (patchReconcile);
//   - "only allow one device to be used in one panel at a time": a device on
//     one Radio is greyed out for every other (drawPatchRadioInspector), and
//     compile() refuses a patch that names one twice (DeviceTwice);
//   - "put all sound to mp3 or wav unless set to speakers or another device":
//     every speaker has an output, a WAV file by default (patchPublishSets);
//   - "when the patch panel is running unpopulate the sdr from the main sdr
//     software so it show signal generator": START on the page switches the
//     receiver to the generator and remembers its radio; STOP, ALL OFF or
//     closing the page opens it again (patchReconcile / patchStopAll).
// And since 0.99.18 each radio has its own ON/OFF switch (Node::on), which
// closes that radio alone while the rest of the patch keeps running.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_devices.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
#include "source/siggen_source.hpp"
#include "source/soapy_source.hpp"

namespace pc = cascade::core::patch;

namespace cascade::gui {

namespace {

constexpr double kPatchDefaultRateHz = 2.0e6;

// The rates offered for a patch radio. Every one either divides to a channel
// rate the plan accepts or is a rate a common radio runs at natively; a radio
// that refuses one keeps its own and says so on its face.
constexpr double kPatchRatesHz[] = {1.0e6,  1.024e6, 2.0e6, 2.048e6, 2.4e6,
                                    2.5e6,  3.0e6,   4.0e6, 6.0e6,   8.0e6, 10.0e6};

double radioRate(const pc::Node& n) { return n.rateHz > 0.0 ? n.rateHz : kPatchDefaultRateHz; }

std::string openedAs(const pc::Node& n) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "@%.17g", radioRate(n));
    return n.device + buf;
}

// Whether an EARLIER radio in the graph already has this node's device - the
// DeviceTwice rule, answered without a compile so the reconcile can use it.
bool deviceTakenEarlier(const pc::Graph& g, const pc::Node& n) {
    for (const pc::Node& other : g.nodes()) {
        if (other.id == n.id) { return false; }
        if (other.kind == pc::NodeKind::Radio && pc::sameDevice(other.device, n.device)) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<cascade::source::SigGenSource> makePatchGenerator(double rateHz, double centreHz) {
    auto g = std::make_unique<cascade::source::SigGenSource>(rateHz);
    // The same picture the receiver's generator draws, so a patch built on
    // the generator shows the carriers the main waterfall does.
    g->sigGen().setTone(0, 300000.0, -30.0f);
    g->sigGen().setTone(1, -500000.0, -45.0f);
    g->sigGen().setNoiseFloorDb(-90.0f);
    if (centreHz > 0.0) { g->setCenterFrequencyHz(centreHz); }
    return g;
}

}  // namespace

std::vector<AppWindow::PatchDeviceChoice> AppWindow::patchDeviceChoices() const {
    std::vector<PatchDeviceChoice> out;
    out.push_back({pc::kGeneratorKey, "Signal generator"});
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // A Pluto needs its address typed in the Source panel; the patch has
        // no field for it, so it is not offered here rather than failing.
        if (d.driver == "pluto") { continue; }
        out.push_back({pc::makeDeviceKey(d.driver, d.args), d.label});
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        out.push_back({pc::makeDeviceKey("soapy", d.args), d.label + " (SoapySDR)"});
    }
    // The receiver's own radio, even when no list currently shows it (a
    // SoapySDR device is only listed after a scan).
    if (patchMainKeep_.valid) {
        const std::string key = pc::makeDeviceKey(patchMainKeep_.kind, patchMainKeep_.args);
        const bool listed = std::any_of(out.begin(), out.end(), [&](const PatchDeviceChoice& c) {
            return c.key == key || pc::sameDevice(c.key, key);
        });
        if (!listed) { out.push_back({key, patchMainKeep_.label + " (the receiver's radio)"}); }
    }
    return out;
}

std::string AppWindow::patchDeviceLabel(const std::string& key) const {
    if (key.empty()) { return "No device chosen"; }
    for (const PatchDeviceChoice& c : patchDeviceChoices()) {
        if (c.key == key) { return c.label; }
    }
    // NOT LISTED IS NOT "NOT CONNECTED": a SoapySDR radio is listed only after
    // a scan. Its args carry its own label, which is what the list would say.
    const std::string args = pc::deviceArgs(key);
    const std::string own = pc::argField(args, "label");
    if (!own.empty()) { return own; }
    const std::string serial = pc::argField(args, "serial");
    return pc::deviceDriver(key) + (serial.empty() ? std::string{} : " " + serial) +
           " (not listed)";
}

std::string AppWindow::patchDefaultDeviceKey() const {
    const auto taken = [this](const std::string& key) {
        for (const pc::Node& n : patchGraph_.nodes()) {
            if (n.kind == pc::NodeKind::Radio && pc::sameDevice(n.device, key)) { return true; }
        }
        return false;
    };
    std::vector<std::string> candidates;
    if (patchMainKeep_.valid) {
        candidates.push_back(pc::makeDeviceKey(patchMainKeep_.kind, patchMainKeep_.args));
    } else if (device_ != nullptr) {
        candidates.push_back(pc::makeDeviceKey(sourceKind_, deviceArgs_));
    }
    for (const PatchDeviceChoice& c : patchDeviceChoices()) {
        if (!pc::isGeneratorKey(c.key)) { candidates.push_back(c.key); }
    }
    for (const std::string& k : candidates) {
        if (!taken(k)) { return k; }
    }
    return pc::kGeneratorKey;
}

std::vector<pc::RadioInfo> AppWindow::patchRadioInfos() const {
    std::vector<pc::RadioInfo> out;
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Radio) { continue; }
        const auto it = patchRadios_.find(n.id);
        if (it != patchRadios_.end()) {
            out.push_back({n.id, it->second->rateHz(), it->second->centreHz()});
        } else {
            // Not running yet: plan against what it is set to, so the patch
            // can be judged before the device has opened.
            out.push_back({n.id, radioRate(n), n.freqHz});
        }
    }
    return out;
}

void AppWindow::patchReconcile() {
    // THE DEVICE LIST IS READ WHEN THE PAGE OPENS. The native list is otherwise
    // read only when the Source combo is opened, and a patch radio named by a
    // saved patch would be labelled "not connected" until then. scanNative()
    // opens nothing and is safe while radios stream (see its comment).
    if (!patchWasOpen_) { scanNative(); }

    // --- the receiver's radio goes to the patch ------------------------------
    // ONLY WHILE THE PATCH RUNS (0.99.18): an open page with the patch stopped
    // leaves the receiver alone, so a patch can be built while listening.
    // Only a live DEVICE is taken (a file or the generator is not a radio),
    // and never mid-open: the answer is waited for, then taken.
    if (patchRunning_ && device_ != nullptr && !deviceOpenPending_) {
        PatchMainKeep keep;
        keep.valid = true;
        keep.kind = sourceKind_;
        keep.args = deviceArgs_;
        keep.label = deviceModel_;
        keep.rateHz = pipeline_.activeSource().sampleRateHz();
        keep.centreHz = pipeline_.activeSource().centerFrequencyHz();
        cascade::core::diagLogf(
            "patch: the receiver's radio (%s) is handed to the patch page; the receiver "
            "runs on the signal generator until the patch is stopped",
            keep.label.c_str());
        selectSource(0);
        // ...AND THE CONFIG STILL NAMES IT: currentConfig() saves the radio
        // held here rather than the generator standing in for it, so a
        // session closed with the page open starts on the radio next time.
        // The Source panel meanwhile says plainly "Signal generator".
        patchMainKeep_ = keep;
        // A patch that has not chosen a device for its radios yet gets this
        // one - the radio the user was just listening to.
        for (const pc::Node& n0 : patchGraph_.nodes()) {
            if (n0.kind != pc::NodeKind::Radio || !n0.device.empty()) { continue; }
            if (pc::Node* n = patchGraph_.mutableNode(n0.id)) {
                n->device = pc::makeDeviceKey(keep.kind, keep.args);
                if (n->freqHz <= 0.0) { n->freqHz = keep.centreHz; }
                if (n->rateHz <= 0.0) { n->rateHz = keep.rateHz; }
                patchUi_.dirty = true;
            }
            break;   // one radio only: the device cannot be on two
        }
    }

    // --- answers from hardware opens -------------------------------------------
    for (auto it = patchRadioPending_.begin(); it != patchRadioPending_.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        const pc::NodeId id = it->first;
        PatchRadioOpen r = it->second.get();
        const std::string as = patchRadioPendingAs_[id];
        it = patchRadioPending_.erase(it);
        patchRadioPendingAs_.erase(id);
        const pc::Node* n = patchGraph_.find(id);
        if (n == nullptr || openedAs(*n) != as || !n->on || !patchRunning_) {
            // The node went, was changed or switched off while the device
            // opened, or the patch stopped: this device is not wanted any
            // more. Dropping r closes it.
            continue;
        }
        if (!r.src) {
            patchRadioError_[id] = r.error.empty() ? "the device would not open" : r.error;
            patchRadioFailedAs_[id] = as;
            cascade::core::diagWarnf("patch: radio '%s' would not open: %s", n->name.c_str(),
                                     patchRadioError_[id].c_str());
            continue;
        }
        auto radio = std::make_unique<pc::PatchRadio>(id, std::move(r.src), r.label);
        std::string err;
        if (!radio->start(err)) {
            patchRadioError_[id] = err;
            patchRadioFailedAs_[id] = as;
            cascade::core::diagWarnf("patch: radio '%s' would not start: %s", n->name.c_str(),
                                     err.c_str());
            continue;
        }
        if (!r.error.empty()) { patchRadioError_[id] = r.error; } else { patchRadioError_.erase(id); }
        patchRadioFailedAs_.erase(id);
        cascade::core::diagLogf("patch: radio '%s' running %s at %.0f S/s, %.6f MHz",
                                n->name.c_str(), r.label.c_str(), radio->rateHz(),
                                radio->centreHz() / 1e6);
        patchRadioOpenedAs_[id] = as;
        patchRadios_[id] = std::move(radio);
        patchRadioSig_.erase(id);
    }

    // --- which radios should run ------------------------------------------------
    // A radio runs while the patch runs AND its own switch is on. A radio
    // switched off forgets why it last failed, so switching it on again is
    // also the way to try a device that would not open.
    std::set<pc::NodeId> wanted;
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Radio || n.device.empty()) { continue; }
        if (!n.on) {
            patchRadioFailedAs_.erase(n.id);
            patchRadioError_.erase(n.id);
            continue;
        }
        if (!patchRunning_) { continue; }
        if (deviceTakenEarlier(patchGraph_, n)) { continue; }   // DeviceTwice: never opened
        wanted.insert(n.id);
    }

    // Stop what is running and not wanted, or wanted differently.
    for (auto it = patchRadios_.begin(); it != patchRadios_.end();) {
        const pc::Node* n = patchGraph_.find(it->first);
        const bool keep = n != nullptr && wanted.count(it->first) != 0 &&
                          patchRadioOpenedAs_[it->first] == openedAs(*n) &&
                          it->second->fault().empty();
        if (keep) {
            ++it;
            continue;
        }
        const pc::NodeId id = it->first;
        if (!it->second->fault().empty()) {
            patchRadioError_[id] = it->second->fault();
            patchRadioFailedAs_[id] = patchRadioOpenedAs_[id];
            cascade::core::diagWarnf("patch: radio stopped: %s", it->second->fault().c_str());
        }
        it->second->stop();
        it = patchRadios_.erase(it);
        patchRadioOpenedAs_.erase(id);
        patchRadioSig_.erase(id);
        patchSpectra_.erase(id);
        patchRefusedBy_.erase(id);
    }

    // Start what is wanted and not running.
    for (const pc::NodeId id : wanted) {
        pc::Node* n = patchGraph_.mutableNode(id);
        if (n == nullptr) { continue; }
        const std::string as = openedAs(*n);
        if (patchRadios_.count(id) != 0) {
            // Running: follow the node's centre, and learn it from the device
            // when the node has none.
            pc::PatchRadio& r = *patchRadios_[id];
            if (n->freqHz <= 0.0) {
                n->freqHz = r.centreHz();
                patchUi_.dirty = true;
            } else if (std::fabs(r.centreHz() - n->freqHz) > 0.5) {
                if (!r.setCentreHz(n->freqHz)) {
                    patchRadioError_[id] = "the device refused that frequency";
                } else {
                    patchRadioError_.erase(id);
                }
            }
            continue;
        }
        if (patchRadioPending_.count(id) != 0) { continue; }   // its answer is coming
        if (patchRadioFailedAs_.count(id) != 0 && patchRadioFailedAs_[id] == as) { continue; }
        const double rate = radioRate(*n);
        const double centre = n->freqHz;
        if (pc::isGeneratorKey(n->device)) {
            auto radio = std::make_unique<pc::PatchRadio>(
                id, makePatchGenerator(rate, centre > 0.0 ? centre : 100.0e6), "Signal generator");
            std::string err;
            if (!radio->start(err)) {
                patchRadioError_[id] = err;
                patchRadioFailedAs_[id] = as;
                continue;
            }
            if (n->freqHz <= 0.0) {
                n->freqHz = radio->centreHz();
                patchUi_.dirty = true;
            }
            patchRadioError_.erase(id);
            patchRadioFailedAs_.erase(id);
            patchRadioOpenedAs_[id] = as;
            patchRadios_[id] = std::move(radio);
            patchRadioSig_.erase(id);
            continue;
        }
        const std::string driver = pc::deviceDriver(n->device);
        const std::string args = pc::deviceArgs(n->device);
        const std::string label = patchDeviceLabel(n->device);
        patchRadioPendingAs_[id] = as;
        patchRadioError_.erase(id);
        patchRadioPending_[id] = std::async(std::launch::async, [driver, args, label, rate,
                                                                 centre]() {
            PatchRadioOpen r;
            r.label = label;
            // SoapySDR's modules are loaded by its enumeration, and the
            // receiver only ever opens a SoapySDR device after its own scan
            // has run one. A patch can name a B200 before any scan this
            // session (a saved patch, the page opened first), and then
            // Device::make() answers "no match" in five milliseconds without
            // looking. Loading the modules - guarded, and without probing any
            // device - is what the scan would have done first.
            if (driver == "soapy") {
                std::string names;
                for (const std::string& d : cascade::source::SoapySource::driverNames()) {
                    names += names.empty() ? d : ", " + d;
                }
                cascade::core::diagLogf("patch: SoapySDR drivers loaded: %s",
                                        names.empty() ? "(none)" : names.c_str());
            }
            const auto openT0 = std::chrono::steady_clock::now();
            std::unique_ptr<cascade::source::DeviceSource> dev = makeDeviceSource(driver);
            if (!dev) {
                r.error = "\"" + driver + "\" is not a device this build can open";
                return r;
            }
            const bool opened = dev->open(args);
            cascade::core::diagLogf(
                "patch: %s open took %.0f ms", driver.c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openT0)
                    .count());
            if (!opened) {
                const char* why = dev->lastError();
                r.error = (why != nullptr && *why != '\0')
                              ? why
                              : "the device would not open - is another program using it?";
                return r;
            }
            // A rate or a centre the radio refuses is not fatal: it keeps its
            // own, the face shows what it is actually running at, and the
            // reason is kept.
            if (!dev->setSampleRateHz(rate)) {
                r.error = "the radio refused " + std::to_string(rate / 1e6).substr(0, 5) +
                          " MS/s and runs at its own rate";
            }
            if (centre > 0.0) { dev->setCenterFrequencyHz(centre); }
            // A patch radio has no gain slider of its own yet, so the radio's
            // own automatic gain is used where it has one - a dongle left at
            // its power-on gain hears very little.
            if (dev->autoGainSupported()) { dev->setAutoGain(true); }
            r.src = std::move(dev);
            return r;
        });
    }

    // --- every radio: retire dead sets, take the newest spectrum ---------------
    for (auto& [id, radio] : patchRadios_) {
        radio->runner().reap();
        PatchSpectrum& s = patchSpectra_[id];
        radio->spectrum(s.db, s.seq);
    }
}

void AppWindow::patchPublishSets() {
    // --- each speaker's output --------------------------------------------------
    std::set<pc::NodeId> live;
    for (const pc::AudioSinkPlan& sp : patchPlan_.sinks) {
        if (patchRadios_.count(sp.radio) == 0) { continue; }   // nothing to write yet
        const pc::Node* n = patchGraph_.find(sp.sink);
        if (n == nullptr) { continue; }
        live.insert(sp.sink);
        const std::string key = n->device.empty() ? std::string("wav") : n->device;
        const auto made = patchDestMadeFor_.find(sp.sink);
        if (made != patchDestMadeFor_.end() && made->second == key) { continue; }

        std::string err;
        std::shared_ptr<pc::AudioDest> dest;
        const std::string prefix = pc::patchFilePrefix(static_cast<unsigned>(sp.sink), n->name);
        switch (pc::outputKind(key)) {
            case pc::OutputKind::Wav:
                dest = pc::makeWavDest(recordDir_, prefix, err);
                break;
            case pc::OutputKind::Mp3: {
                dest = pc::makeMp3Dest(recordDir_, prefix, err);
                if (!dest) {
                    // THE FILE STILL GETS WRITTEN: an MP3 this build cannot
                    // make is a WAV, and the face says so.
                    std::string werr;
                    dest = pc::makeWavDest(recordDir_, prefix, werr);
                    err += dest ? " - writing WAV instead" : "; " + werr;
                }
                break;
            }
            case pc::OutputKind::Speakers:
                dest = pc::makeDeviceDest(std::string{}, err);
                break;
            case pc::OutputKind::Device:
                dest = pc::makeDeviceDest(pc::outputDeviceName(key), err);
                break;
        }
        bool replaced = false;
        for (auto& d : patchDests_) {
            if (d.first == sp.sink) {
                d.second = dest;
                replaced = true;
            }
        }
        if (!replaced) { patchDests_.emplace_back(sp.sink, dest); }
        patchDestMadeFor_[sp.sink] = key;
        if (err.empty()) {
            patchDestError_.erase(sp.sink);
        } else {
            patchDestError_[sp.sink] = err;
        }
        cascade::core::diagLogf("patch: speaker '%s' -> %s%s%s", n->name.c_str(),
                                dest ? dest->describe().c_str() : "nothing",
                                err.empty() ? "" : " - ", err.c_str());
    }
    // Speakers that stopped playing lose their output. The file is finalised
    // when the running set that still holds it is retired.
    for (auto it = patchDests_.begin(); it != patchDests_.end();) {
        if (live.count(it->first) != 0) {
            ++it;
            continue;
        }
        patchDestMadeFor_.erase(it->first);
        patchDestError_.erase(it->first);
        it = patchDests_.erase(it);
    }

    // --- each radio's set -------------------------------------------------------
    for (auto& [id, radio] : patchRadios_) {
        const double rate = radio->rateHz();
        const std::string sig = pc::radioSignature(patchPlan_, patchGraph_, id, rate,
                                                   &patchCatalogue_, &patchApis_, patchDests_);
        const auto have = patchRadioSig_.find(id);
        if (have != patchRadioSig_.end() && have->second == sig) { continue; }
        std::shared_ptr<pc::StripSet> set = pc::buildRadioSet(
            patchPlan_, patchGraph_, id, rate, &patchCatalogue_, &patchApis_, patchDests_);
        patchRefusedBy_[id] = set->refused;
        for (const auto& d : set->decoders) {
            patchDecoderFaces_.erase(d->node);
            patchFirstLineLogged_.erase(d->node);
            cascade::core::diagLogf("patch: decoder '%s' started on radio %u at %.0f Hz",
                                    d->name.c_str(), static_cast<unsigned>(id), d->rateHz);
        }
        for (const pc::NodeId r : set->refused) {
            const pc::Node* n = patchGraph_.find(r);
            cascade::core::diagLogf("patch: decoder '%s' (%s) refused to start",
                                    n != nullptr ? n->name.c_str() : "?",
                                    n != nullptr ? n->plugin.c_str() : "?");
        }
        radio->runner().publish(std::move(set));
        patchRadioSig_[id] = sig;
    }
    // Every demodulator's squelch, live - after any publish, so a new set's
    // channels get the setting from their first block.
    patchPushSquelch();
    patchRefused_.clear();
    for (const auto& [id, list] : patchRefusedBy_) {
        (void)id;
        patchRefused_.insert(patchRefused_.end(), list.begin(), list.end());
    }
}

void AppWindow::patchStopAll(bool restoreMain) {
    for (auto& [id, radio] : patchRadios_) {
        (void)id;
        radio->stop();
    }
    // Destroying the radios destroys their runners and every set in them - on
    // this thread, after the readers have stopped - which finalises each
    // speaker's file and destroys each decoder handle where the ABI wants it.
    patchRadios_.clear();
    patchRadioOpenedAs_.clear();
    patchRadioSig_.clear();
    patchSpectra_.clear();
    patchRefusedBy_.clear();
    patchRadioError_.clear();
    patchRadioFailedAs_.clear();
    patchDests_.clear();
    patchDestMadeFor_.clear();
    patchDestError_.clear();
    // An open still in flight is NOT waited on here - a USB walk can take
    // seconds and this is the GUI thread. Its future moves to a thread of its
    // own that waits for the answer and drops it, which closes the device.
    for (auto& [id, fut] : patchRadioPending_) {
        (void)id;
        std::thread([f = std::move(fut)]() mutable {
            if (f.valid()) { (void)f.get(); }
        }).detach();
    }
    patchRadioPending_.clear();
    patchRadioPendingAs_.clear();

    if (!restoreMain || !patchMainKeep_.valid) { return; }
    // --- the receiver gets its radio back ---------------------------------------
    const PatchMainKeep keep = patchMainKeep_;
    patchMainKeep_ = PatchMainKeep{};
    int row = -1;
    if (keep.kind == "soapy") {
        for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
            if (soapyDevices_[i].args == keep.args) { row = soapyRowBase() + static_cast<int>(i); }
        }
    } else {
        for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
            if (nativeDevices_[i].driver == keep.kind && nativeDevices_[i].args == keep.args) {
                row = kNativeRowBase + static_cast<int>(i);
            }
        }
    }
    if (row < 0) {
        sourceError_ = "the radio the patch page was using (" + keep.label +
                       ") is not listed any more - choose it again in Source";
        cascade::core::diagWarnf("patch: could not hand %s back to the receiver - not listed",
                                 keep.label.c_str());
        return;
    }
    // Back where it was tuned when the patch took it.
    if (keep.centreHz > 0.0) { pipeline_.activeSource().setCenterFrequencyHz(keep.centreHz); }
    cascade::core::diagLogf("patch: handing %s back to the receiver", keep.label.c_str());
    selectSource(row);
}

void AppWindow::patchPressStart() {
    if (patchRunning_) {
        patchRunning_ = false;
        return;
    }
    // START WITH EVERY RADIO SWITCHED OFF SWITCHES THEM ALL ON (see
    // switchOnForStart for why).
    if (pc::switchOnForStart(patchGraph_)) { patchUi_.dirty = true; }
    patchRunning_ = true;
}

void AppWindow::patchAllOff() {
    // EVERY RADIO OFF, and the patch stopped: the receiver gets its radio back
    // when patchApplyRunning sees the change.
    if (pc::switchAllRadiosOff(patchGraph_)) { patchUi_.dirty = true; }
    patchRunning_ = false;
}

void AppWindow::patchApplyRunning() {
    if (patchRunning_ == patchWasRunning_) { return; }
    patchWasRunning_ = patchRunning_;
    if (patchRunning_) {
        cascade::core::diagLogf("patch: START - the patch's radios open");
    } else {
        cascade::core::diagLogf("patch: STOP - every patch radio closes");
        // Every radio stops, every speaker's file is finalised, and the
        // receiver gets its radio back.
        patchStopAll(true);
    }
    // The receiver's decoders stand down while the patch runs and come back
    // when it stops.
    refreshPluginRunner();
}

void AppWindow::drawPatchTransport() {
    // THE SAME TRANSPORT AS THE MAIN PANEL (owner, 0.99.18: "a start button
    // like we have on the main panel so the user doesnt have to go between the
    // panels"). The dome is the receiver's own drawBenchStopButton, lettered
    // from the same bool it acts on, so it cannot say START while running.
    constexpr float kR = 24.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 centre(at.x + kR * 1.1f, at.y + kR * 1.1f);
    if (drawBenchStopButton(dl, centre, kR, patchRunning_)) { patchPressStart(); }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(patchRunning_
                              ? "Stop the patch: every patch radio closes, recordings are\n"
                                "finished, and the receiver gets its radio back."
                              : "Start the patch: every radio switched on opens. The receiver\n"
                                "hands its radio over and runs on the signal generator.");
    }

    // ALL OFF: larger, red, and always there. Every radio's switch goes off
    // and the patch stops.
    ImGui::SetCursorScreenPos(ImVec2(at.x + kR * 2.5f, at.y + kR * 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Button, cascade::gui::theme::kAlarm);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, cascade::gui::theme::kAlarmHot);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, cascade::gui::theme::kAlarmHot);
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::kIvory);
    if (ImGui::Button("ALL OFF###patchalloff", ImVec2(150.0f, kR * 1.7f))) { patchAllOff(); }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Switch every radio off and stop the patch. START switches\n"
                          "them all on again.");
    }

    // What is running, in words.
    std::size_t radios = 0, on = 0;
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Radio) { continue; }
        ++radios;
        if (n.on) { ++on; }
    }
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(
        ImVec2(ImGui::GetCursorScreenPos().x + 8.0f, at.y + kR * 1.1f - ImGui::GetTextLineHeight() * 0.5f));
    char line[96];
    if (patchRunning_) {
        std::snprintf(line, sizeof(line), "RUNNING - %zu of %zu radio%s open", patchRadios_.size(),
                      radios, radios == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
    } else {
        std::snprintf(line, sizeof(line), "STOPPED - %zu of %zu radio%s switched on", on, radios,
                      radios == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
    }
    ImGui::TextUnformatted(line);
    ImGui::PopStyleColor();

    // Below the dome, for whatever comes next on the page.
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + kR * 2.3f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void AppWindow::drawPatchRadioSwitch(pc::Node& n) {
    // LIT while this radio is actually open, amber while it is switched on and
    // waiting (the patch stopped, or the device still opening), dark when off.
    const bool live = patchRadios_.count(n.id) != 0;
    const ImU32 face = !n.on ? cascade::gui::theme::kEnamel
                       : live ? cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.45f)
                              : cascade::gui::theme::withAlpha(cascade::gui::theme::kAmber, 0.35f);
    ImGui::PushStyleColor(ImGuiCol_Button, face);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          cascade::gui::theme::withAlpha(cascade::gui::theme::kIvory, 0.25f));
    if (ImGui::SmallButton(n.on ? "ON###radioOn" : "OFF###radioOn")) {
        n.on = !n.on;
        patchUi_.dirty = true;
        cascade::core::diagLogf("patch: radio '%s' switched %s", n.name.c_str(), n.on ? "on" : "off");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(n.on ? "This radio is switched on. Press to close it alone - the rest\n"
                                 "of the patch keeps running."
                               : "This radio is switched off. Press to switch it on; it opens\n"
                                 "while the patch is running.");
    }
}

namespace {

// The Channel a Demod node is fed from, or kNoNode.
pc::NodeId demodChannel(const pc::Graph& g, pc::NodeId demod) {
    for (const pc::Wire& w : g.wires()) {
        if (w.to != demod) { continue; }
        const pc::Node* f = g.find(w.from);
        return (f != nullptr && f->kind == pc::NodeKind::Channel) ? f->id : pc::kNoNode;
    }
    return pc::kNoNode;
}

}  // namespace

void AppWindow::patchCollectMapTargets(pc::NodeId map) {
    patchMapTracks_.clear();
    patchMapPaths_.clear();
    // Targets are tagged with their module's display name by the plugin UI,
    // which already applies the host's staleness rule - the same list the map
    // pages draw, so a patch map and a map page never disagree about what is
    // there.
    const std::vector<std::string> sources = pc::mapSources(patchGraph_, map, patchCatalogue_);
    if (sources.empty()) { return; }
    const auto wanted = [&sources](const std::string& plugin) {
        return std::find(sources.begin(), sources.end(), plugin) != sources.end();
    };
    for (const cascade::core::HostTrack& t : pluginUi_.tracks()) {
        if (wanted(t.plugin)) { patchMapTracks_.push_back(t); }
    }
    for (const cascade::core::HostPath& p : pluginUi_.paths()) {
        if (wanted(p.plugin)) { patchMapPaths_.push_back(p); }
    }
}

void AppWindow::drawPatchMapInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    const auto amber = cascade::gui::theme::vec(cascade::gui::theme::kAmber);
    ImGui::Spacing();
    std::size_t wired = 0;
    for (const pc::Wire& w : patchGraph_.wires()) {
        if (w.to == n.id) { ++wired; }
    }
    ImGui::Text("%zu of %zu inputs wired", wired, pc::kMapInputs);
    patchCollectMapTargets(n.id);
    for (const std::string& src : pc::mapSources(patchGraph_, n.id, patchCatalogue_)) {
        std::size_t count = 0;
        for (const cascade::core::HostTrack& t : patchMapTracks_) {
            if (t.plugin == src) { ++count; }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::Text("%zu", count);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", src.c_str());
    }
    const auto view = patchMapViews_.find(n.id);
    if (view != patchMapViews_.end() && view->second) {
        if (ImGui::Button("Fit to targets", ImVec2(-FLT_MIN, 0.0f))) {
            view->second->requestFitToTracks();
        }
        if (ImGui::Button("Whole world", ImVec2(-FLT_MIN, 0.0f))) {
            view->second->requestWholeWorld();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped(
        "Wire the map output of up to %zu decoders here - aircraft from one radio, ships from "
        "another - and they share this map. Drag it to pan, scroll it to zoom.",
        pc::kMapInputs);
    ImGui::PopStyleColor();
}

void AppWindow::patchPushSquelch() {
    for (const pc::Node& n : patchGraph_.nodes()) {
        if (n.kind != pc::NodeKind::Demod) { continue; }
        const pc::NodeId chan = demodChannel(patchGraph_, n.id);
        if (chan == pc::kNoNode) { continue; }
        const auto r = patchRadios_.find(pc::radioOf(patchGraph_, chan));
        if (r == patchRadios_.end()) { continue; }
        r->second->runner().setSquelchDb(chan, n.squelch ? n.squelchDb : pc::kSquelchOffDb);
    }
}

void AppWindow::drawPatchSquelch(pc::Node& n, float width) {
    bool on = n.squelch;
    if (ImGui::Checkbox("Squelch##sq", &on)) {
        n.squelch = on;
        patchUi_.dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Silences this demodulator's sound while the channel is quieter\n"
                          "than the threshold - so a speaker or a recording hears signals,\n"
                          "not the noise between them. Decoders are still given every sample.");
    }
    if (n.squelch) {
        float db = n.squelchDb;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderFloat("##sqdb", &db, pc::kSquelchMinDb, pc::kSquelchMaxDb, "%.0f dB")) {
            n.squelchDb = db;
            patchUi_.dirty = true;
        }
    }
    // WHERE TO SET IT: the level the gate is judging, and whether it is open.
    const pc::NodeId chan = demodChannel(patchGraph_, n.id);
    const auto r = patchRadios_.find(pc::radioOf(patchGraph_, chan));
    float level = 0.0f;
    bool open = false;
    if (chan != pc::kNoNode && r != patchRadios_.end() &&
        r->second->runner().squelchState(chan, level, open)) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::Text("%.0f dB", static_cast<double>(level));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (!n.squelch || open) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
            ImGui::TextUnformatted(n.squelch ? "open" : "no squelch");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
            ImGui::TextUnformatted("closed");
        }
        ImGui::PopStyleColor();
    }
}

void AppWindow::drawPatchRadioInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    const auto amber = cascade::gui::theme::vec(cascade::gui::theme::kAmber);

    ImGui::Spacing();
    ImGui::TextUnformatted("Device");
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::vector<PatchDeviceChoice> choices = patchDeviceChoices();
    if (ImGui::BeginCombo("##patchdevice", patchDeviceLabel(n.device).c_str())) {
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const PatchDeviceChoice& c = choices[i];
            // ONE DEVICE, ONE RADIO: a device another Radio already has is
            // shown, greyed, with who has it - not hidden, so the user can see
            // why it is not offered.
            const pc::Node* holder = nullptr;
            for (const pc::Node& other : patchGraph_.nodes()) {
                if (other.id != n.id && other.kind == pc::NodeKind::Radio &&
                    pc::sameDevice(other.device, c.key)) {
                    holder = &other;
                }
            }
            ImGui::PushID(static_cast<int>(i));
            std::string text = c.label;
            if (holder != nullptr) { text += "  (used by " + holder->name + ")"; }
            ImGui::BeginDisabled(holder != nullptr);
            if (ImGui::Selectable(text.c_str(), c.key == n.device)) {
                n.device = c.key;
                patchUi_.dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (soapyDevices_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextWrapped("SoapySDR radios appear here after a scan in SIGNAL PATH > Source.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Centre (MHz)");
    double mhz = n.freqHz / 1e6;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputDouble("##patchcentre", &mhz, 0.1, 1.0, "%.6f",
                           ImGuiInputTextFlags_EnterReturnsTrue) &&
        mhz > 0.0) {
        n.freqHz = mhz * 1e6;
        patchUi_.dirty = true;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Sample rate");
    char rateText[32];
    std::snprintf(rateText, sizeof(rateText), "%.3f MS/s", radioRate(n) / 1e6);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##patchrate", rateText)) {
        for (const double r : kPatchRatesHz) {
            char t[32];
            std::snprintf(t, sizeof(t), "%.3f MS/s", r / 1e6);
            if (ImGui::Selectable(t, std::fabs(r - radioRate(n)) < 1.0)) {
                n.rateHz = r;
                patchUi_.dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    // What it is actually doing.
    ImGui::Spacing();
    const auto run = patchRadios_.find(n.id);
    if (run != patchRadios_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::Text("running %.3f MS/s", run->second->rateHz() / 1e6);
        ImGui::Text("at %.6f MHz", run->second->centreHz() / 1e6);
        ImGui::PopStyleColor();
    } else if (patchRadioPending_.count(n.id) != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted("opening the device...");
        ImGui::PopStyleColor();
    }
    const auto err = patchRadioError_.find(n.id);
    if (err != patchRadioError_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
        ImGui::TextWrapped("%s", err->second.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped("Up to %zu radios, each its own device. While this page is open the "
                       "receiver runs on the signal generator; closing it gives the radio back.",
                       pc::kMaxRadios);
    ImGui::PopStyleColor();
}

void AppWindow::drawPatchSinkInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    ImGui::Spacing();
    ImGui::TextUnformatted("Sound goes to");
    const std::string key = n.device.empty() ? std::string("wav") : n.device;
    std::string current;
    switch (pc::outputKind(key)) {
        case pc::OutputKind::Wav: current = "A WAV file"; break;
        case pc::OutputKind::Mp3: current = "An MP3 file"; break;
        case pc::OutputKind::Speakers: current = "The speakers"; break;
        case pc::OutputKind::Device: current = pc::outputDeviceName(key); break;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##patchoutput", current.c_str())) {
        if (ImGui::Selectable("A WAV file", pc::outputKind(key) == pc::OutputKind::Wav)) {
            n.device = "wav";
            patchUi_.dirty = true;
        }
        const bool mp3 = cascade::core::Mp3Writer::available();
        ImGui::BeginDisabled(!mp3);
        if (ImGui::Selectable(mp3 ? "An MP3 file" : "An MP3 file (needs Windows)",
                              pc::outputKind(key) == pc::OutputKind::Mp3)) {
            n.device = "mp3";
            patchUi_.dirty = true;
        }
        ImGui::EndDisabled();
        if (ImGui::Selectable("The speakers", pc::outputKind(key) == pc::OutputKind::Speakers)) {
            n.device = "speakers";
            patchUi_.dirty = true;
        }
        for (std::size_t i = 0; i < devices_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const std::string k = pc::makeOutputDeviceKey(devices_[i].name);
            if (ImGui::Selectable(devices_[i].name.c_str(), key == k)) {
                n.device = k;
                patchUi_.dirty = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    for (const auto& d : patchDests_) {
        if (d.first != n.id || !d.second) { continue; }
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
        ImGui::TextWrapped("%s", d.second->describe().c_str());
        ImGui::Text("%.1f s", static_cast<double>(d.second->samples()) / pc::kOutRateHz);
        ImGui::PopStyleColor();
        const std::string e = d.second->error();
        if (!e.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
            ImGui::TextWrapped("%s", e.c_str());
            ImGui::PopStyleColor();
        }
    }
    const auto err = patchDestError_.find(n.id);
    if (err != patchDestError_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
        ImGui::TextWrapped("%s", err->second.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    if (pc::outputKind(key) == pc::OutputKind::Wav || pc::outputKind(key) == pc::OutputKind::Mp3) {
        ImGui::TextWrapped("Files go in %s, one per speaker, named after it.", recordDir_.c_str());
    }
    ImGui::PopStyleColor();
}

}  // namespace cascade::gui
