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
#include <cstdlib>
#include <filesystem>
#include <set>
#include <thread>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/mp3_writer.hpp"
#include "core/patch_audio.hpp"
#include "core/patch_devices.hpp"
#include "gui/rate_follow_status.hpp"
#include "gui/scope_face.hpp"
#include "gui/soundcard_panel.hpp"
#include "gui/theme.hpp"
#include "source/siggen_source.hpp"
#include "source/soapy_source.hpp"

namespace pc = cascade::core::patch;

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

constexpr double kPatchDefaultRateHz = 2.0e6;

// The rates offered for a patch radio. Every one either divides to a channel
// rate the plan accepts or is a rate a common radio runs at natively; a radio
// that refuses one keeps its own and says so on its face.
constexpr double kPatchRatesHz[] = {1.0e6,  1.024e6, 2.0e6, 2.048e6, 2.4e6,
                                    2.5e6,  3.0e6,   4.0e6, 6.0e6,   8.0e6, 10.0e6};

double radioRate(const pc::Node& n) { return n.rateHz > 0.0 ? n.rateHz : kPatchDefaultRateHz; }

// WHAT THE PATCH LOGS. Nodes are named by the user, and a radio's label ends
// in its serial, so a log line names a node by its NUMBER and a radio by its
// model - and never says what it is tuned to. Every uploaded line is also
// scrubbed (core::scrubUploadLog); this keeps the local log to the same rule.
std::string modelOnly(const std::string& label) {
    const std::size_t at = label.find(" (serial ");
    return at == std::string::npos ? label : label.substr(0, at);
}

// A speaker's destination by KIND: the WAV/MP3 file is named after the node,
// which the user named.
const char* destKind(const pc::AudioDest* dest) {
    if (dest == nullptr) { return "nothing"; }
    const std::string d = dest->describe();
    if (d.rfind("WAV", 0) == 0) { return "a WAV file"; }
    if (d.rfind("MP3", 0) == 0) { return "an MP3 file"; }
    return "an output device";
}

// What the radio was opened AS: its device and, for a radio, its rate. A
// recording's rate is the file's, so it is left out (pc::radioOpenIdentity) -
// the node taking the file's rate must not reopen it.
std::string openedAs(const pc::Node& n) { return pc::radioOpenIdentity(n.device, radioRate(n)); }

// "2.000 MS/s", "48.000 kS/s": a recording's rate as its list row says it.
std::string recordingRateText(double hz) {
    char buf[32];
    if (hz >= 1.0e6) {
        std::snprintf(buf, sizeof(buf), "%.3f MS/s", hz / 1.0e6);
    } else {
        std::snprintf(buf, sizeof(buf), "%.3f kS/s", hz / 1.0e3);
    }
    return buf;
}

// The file name of a recording key, for its label - as UTF-8 for the screen.
std::string recordingFileName(const std::string& key) {
    try {
        const auto s = std::filesystem::path(pc::iqFilePath(key)).filename().u8string();
        return std::string(s.begin(), s.end());
    } catch (...) {
        return pc::iqFilePath(key);
    }
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
    out.push_back({pc::kGeneratorKey, tr("Signal generator")});
    // THE RECORDINGS, beside the generator: like it, they are not hardware,
    // and a patch can be built and decoded on them with no radio on the desk.
    for (const pc::RecordingInfo& r : patchRecordings_) {
        std::string label;
        cascade::core::formatUtf8(label, tr("Recording: %s"), r.fileName.c_str());
        out.push_back({r.key, label + " (" + recordingRateText(r.rateHz) + ")"});
    }
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // A Pluto needs its address typed in the Source panel; the patch has
        // no field for it, so it is not offered here rather than failing.
        if (d.driver == "pluto") { continue; }
        out.push_back({pc::makeDeviceKey(d.driver, d.args), d.label});
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        // NOT WRAPPED: "(SoapySDR)" is only the excluded product name in
        // parentheses, nothing else to translate.
        out.push_back({pc::makeDeviceKey("soapy", d.args), d.label + " (SoapySDR)"});
    }
    // Every sound card input the Source section has listed (the list is asked
    // for when the patch page is open - see pollSoundCard).
    for (const cascade::source::SoundCardDevice& d : soundCardDevices_) {
        out.push_back({pc::makeDeviceKey("soundcard", cascade::source::soundCardDeviceArgs(d.name, d.hostApi)),
                       std::string(tr("Sound card")) + ": " + cascade::source::soundCardDeviceLabel(d)});
    }
    // The receiver's own radio, even when no list currently shows it (a
    // SoapySDR device is only listed after a scan).
    if (patchMainKeep_.valid) {
        const std::string key = pc::makeDeviceKey(patchMainKeep_.kind, patchMainKeep_.args);
        const bool listed = std::any_of(out.begin(), out.end(), [&](const PatchDeviceChoice& c) {
            return c.key == key || pc::sameDevice(c.key, key);
        });
        if (!listed) {
            std::string buf;
            cascade::core::formatUtf8(buf, tr("%s (the receiver's radio)"),
                          patchMainKeep_.label.c_str());
            out.push_back({key, buf});
        }
    }
    return out;
}

void AppWindow::patchListRecordings() {
    std::vector<std::string> dirs{recordDir_};
    // A second folder for verification and for anyone who keeps recordings
    // elsewhere - read from the environment, never guessed.
    if (const char* s = std::getenv("FOXSDR_PATCH_SAMPLES"); s != nullptr && *s != '\0') {
        dirs.emplace_back(s);
    }
    patchRecordings_ = pc::listIqRecordings(dirs);
    cascade::core::diagLogf("patch: %zu I/Q recording(s) listed", patchRecordings_.size());
}

std::string AppWindow::patchDeviceLabel(const std::string& key) const {
    if (key.empty()) { return tr("No device chosen"); }
    for (const PatchDeviceChoice& c : patchDeviceChoices()) {
        if (c.key == key) { return c.label; }
    }
    // A RECORDING IS NAMED BY ITS FILE whether or not the list has been read:
    // the key carries the path, and the list is only read when asked for.
    if (pc::isIqFileKey(key)) {
        std::string buf;
        cascade::core::formatUtf8(buf, tr("Recording: %s"), recordingFileName(key).c_str());
        return buf;
    }
    // NOT LISTED IS NOT "NOT CONNECTED": a SoapySDR radio is listed only after
    // a scan. Its args carry its own label, which is what the list would say.
    const std::string args = pc::deviceArgs(key);
    const std::string own = pc::argField(args, "label");
    if (!own.empty()) { return own; }
    const std::string serial = pc::argField(args, "serial");
    std::string buf;
    if (serial.empty()) {
        cascade::core::formatUtf8(buf, tr("%s (not listed)"), pc::deviceDriver(key).c_str());
    } else {
        cascade::core::formatUtf8(buf, tr("%s %s (not listed)"), pc::deviceDriver(key).c_str(),
                      serial.c_str());
    }
    return buf;
}

bool AppWindow::setPatchRadioCentre(pc::Node& n, double airHz) {
    // THE RADIO decides, not the sign of the air figure: a node's centre is an
    // AIR frequency and may be carried in below 0 Hz through an up-converter,
    // so refusing everything <= 0 here made such a centre impossible to type
    // back in. What must hold is that the radio behind the node's converter
    // is told something above 0 Hz.
    const cascade::core::ConverterSetting conv = converterForKey(n.device);
    if (!cascade::core::radioCentreTakeable(conv, airHz)) {
        std::string msg;
        if (cascade::core::converterActive(conv)) {
            cascade::core::formatUtf8(
                msg, tr("%s is out of reach through the %s: the radio would have to tune to 0 Hz or below."),
                cascade::core::converterHzText(airHz).c_str(), converterName(conv).c_str());
        } else {
            cascade::core::formatUtf8(msg, tr("A radio cannot tune to %s - type a centre above 0 Hz."),
                                      cascade::core::converterHzText(airHz).c_str());
        }
        patchCentreNote_[n.id] = msg;
        return false;
    }
    n.freqHz = airHz;
    n.centreChosen = true;   // 0 Hz on the air is a centre too
    patchCentreNote_.erase(n.id);
    patchUi_.dirty = true;
    return true;
}

void AppWindow::drawPatchCentreNote(const pc::Node& n) {
    const auto it = patchCentreNote_.find(n.id);
    if (it == patchCentreNote_.end()) { return; }
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::warning());
    ImGui::TextWrapped("%s", it->second.c_str());
    ImGui::PopStyleColor();
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
        // Radios only: a new Radio is never started on a recording nobody
        // chose - the generator is the stand-in when no radio is free.
        if (!pc::isGeneratorKey(c.key) && !pc::isIqFileKey(c.key)) { candidates.push_back(c.key); }
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
    //
    // AND THE SOAPYSDR LIST TOO (2026-09-23). A B200 is only found by the
    // SoapySDR scan, which the page never asked for: the owner's B200 did not
    // appear here until it had been opened in the receiver. scanSoapy() now
    // runs beside open radios, leaving their own drivers out, so asking here
    // is safe with the receiver or the patch streaming.
    //
    // WANTED, NOT FIRED ONCE: the page often opens while a radio is still
    // opening (a session restoring its source), when the plan must defer - and
    // a scan asked for only on the first frame then never happened. So the
    // wish is kept and the scan runs on the first frame the plan allows.
    //
    // NOT ON SHOWING THE VIEW ANY MORE (0.99.40). The patch is now the view
    // the application opens on and is switched to and fro all day, and a
    // SoapySDR scan asked for here would run the vendor probe - which opens
    // and resets USB radios - at every launch and every switch, the very
    // thing the constructor refuses to do. So the view's first frame reads
    // the native list (it opens nothing), and the SoapySDR scan waits for the
    // user to ask for a list: a Radio's device list opened (it asks once, as
    // the Source section's does) or "Look for radios".
    if (!patchWasOpen_) { scanNative(); }

    // --- the receiver's radio goes to the patch ------------------------------
    // ONLY WHILE THE PATCH RUNS (0.99.18): an open page with the patch stopped
    // leaves the receiver alone, so a patch can be built while listening.
    // Only a live DEVICE is taken (a file or the generator is not a radio),
    // and never mid-open: the answer is waited for, then taken. The
    // receiver's SOUND CARD is lent the same way (gui::receiverSourceForPatch):
    // left with the receiver, a patch radio on the same card would open a
    // second stream on it, which WASAPI exclusive mode refuses outright. The
    // card is described by what is RUNNING (soundCardLive_), never by the
    // Source section's controls, which may have been edited and not Opened.
    const cascade::gui::ReceiverLoan loan = cascade::gui::receiverSourceForPatch(
        patchRunning_, sourceKind_, device_ != nullptr, deviceOpenPending_, deviceArgs_,
        soundCardOpenPending_, soundCardLive_);
    if (loan.take) {
        PatchMainKeep keep;
        keep.valid = true;
        keep.kind = loan.kind;
        keep.args = loan.args;
        keep.card = loan.card;
        keep.label = loan.kind == "soundcard"
                         ? std::string(tr("Sound card")) + ": " + loan.card.device + " (" +
                               loan.card.hostApi + ")"
                         : deviceModel_;
        keep.rateHz = pipeline_.activeSource().sampleRateHz();
        // The AIR centre, which may be below 0 Hz through a converter; no
        // value only when the radio has never been tuned.
        keep.centreHz = carriedAirCentre();
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
                // A node with a centre of its own keeps it. The carried one is
                // marked chosen, never judged by its value: 0 Hz on the air is
                // a centre (see pc::Node::centreChosen).
                if (!pc::radioCentreSet(*n) && keep.centreHz.has_value()) {
                    n->freqHz = *keep.centreHz;
                    n->centreChosen = true;
                }
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
            cascade::core::diagWarnf("patch: radio node %u would not open: %s",
                                     static_cast<unsigned>(id), patchRadioError_[id].c_str());
            continue;
        }
        auto radio = std::make_unique<pc::PatchRadio>(id, std::move(r.src), r.label);
        // THE SAME CONVERTER the receiver uses for this device (keyed alike:
        // core::converterRadioKey IS the patch device key), so the node's
        // frequency is an air frequency here too.
        radio->setConverter(converterForKey(n->device));
        std::string err;
        if (!radio->start(err)) {
            patchRadioError_[id] = err;
            patchRadioFailedAs_[id] = as;
            cascade::core::diagWarnf("patch: radio node %u would not start: %s",
                                     static_cast<unsigned>(id), err.c_str());
            continue;
        }
        if (!r.error.empty()) { patchRadioError_[id] = r.error; } else { patchRadioError_.erase(id); }
        patchRadioFailedAs_.erase(id);
        cascade::core::diagLogf("patch: radio node %u running %s at %.0f S/s",
                                static_cast<unsigned>(id), modelOnly(r.label).c_str(),
                                radio->rateHz());
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
            // A converter changed in the Source section while this radio runs
            // takes effect here; the follow below then retunes the radio so
            // the node's AIR frequency is what it hears.
            const cascade::core::ConverterSetting conv = converterForKey(n->device);
            if (r.converter() != conv) { r.setConverter(conv); }
            if (!pc::radioCentreSet(*n)) {
                n->freqHz = r.centreHz();
                n->centreChosen = true;
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
        // The node's frequency is AIR; the device is told it through the
        // converter remembered for it (off unless the user set one there).
        const cascade::core::ConverterSetting conv = converterForKey(n->device);
        // AN I/Q RECORDING (0.99.40), opened here on the GUI thread as the
        // Source section opens one: a header read, bounded, and no USB walk
        // to wait for. Its RATE is the file's, and the node takes it - which
        // does not reopen it (openedAs leaves a recording's rate out). Its
        // CENTRE is the node's frequency: the air frequency the recording is
        // baseband around, told to the file as the receiver's file is told
        // one (through the "file" converter, off unless the user set it).
        if (pc::isIqFileKey(n->device)) {
            const double radioHz =
                (pc::radioCentreSet(*n) && cascade::core::airReachable(conv, centre))
                    ? cascade::core::radioFromAir(conv, centre)
                    : 0.0;
            std::string err;
            std::unique_ptr<cascade::source::IqFileSource> file =
                pc::openIqRecording(n->device, radioHz, err);
            if (!file) {
                patchRadioError_[id] = err;
                patchRadioFailedAs_[id] = as;
                // The NODE, never the file's name: a path is the user's data.
                cascade::core::diagWarnf("patch: radio node %u: its I/Q recording would not open",
                                         static_cast<unsigned>(id));
                continue;
            }
            if (std::fabs(n->rateHz - file->sampleRateHz()) > 0.5) {
                n->rateHz = file->sampleRateHz();
                patchUi_.dirty = true;
            }
            auto radio = std::make_unique<pc::PatchRadio>(id, std::move(file), "I/Q recording");
            radio->setConverter(conv);
            std::string serr;
            if (!radio->start(serr)) {
                patchRadioError_[id] = serr;
                patchRadioFailedAs_[id] = as;
                continue;
            }
            if (!pc::radioCentreSet(*n)) {
                n->freqHz = radio->centreHz();
                n->centreChosen = true;
                patchUi_.dirty = true;
            }
            cascade::core::diagLogf("patch: radio node %u playing an I/Q recording at %.0f S/s",
                                    static_cast<unsigned>(id), radio->rateHz());
            patchRadioError_.erase(id);
            patchRadioFailedAs_.erase(id);
            patchRadioOpenedAs_[id] = openedAs(*n);
            patchRadios_[id] = std::move(radio);
            patchRadioSig_.erase(id);
            continue;
        }
        if (pc::isGeneratorKey(n->device)) {
            auto radio = std::make_unique<pc::PatchRadio>(
                id,
                makePatchGenerator(rate, cascade::core::radioFromAir(
                                             conv, pc::radioCentreSet(*n) ? centre : 100.0e6)),
                "Signal generator");
            radio->setConverter(conv);
            std::string err;
            if (!radio->start(err)) {
                patchRadioError_[id] = err;
                patchRadioFailedAs_[id] = as;
                continue;
            }
            if (!pc::radioCentreSet(*n)) {
                n->freqHz = radio->centreHz();
                n->centreChosen = true;
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
        // A sound card opens as the Source section has it set up when it is
        // the same card (format, channel, centre), plain real mono otherwise.
        const std::string args = driver == "soundcard" ? soundCardPatchArgs(pc::deviceArgs(n->device))
                                                       : pc::deviceArgs(n->device);
        // NOT UNDER A SCAN THAT MAY PROBE IT (2026-09-23). The Source panel
        // greys itself out while a scan runs, so the receiver never opens a
        // radio under one; the patch opens its own radios and did not wait -
        // and a scan probing the dongle being opened is the 0.90.0 fault. It
        // waits for the scan and is started on the frame after it ends.
        if (soapyScanPending_ && cascade::gui::scanMayProbe(soapyScanSkip_, driver, args)) {
            continue;
        }
        const std::string label = patchDeviceLabel(n->device);
        patchRadioPendingAs_[id] = as;
        patchRadioError_.erase(id);
        // Converted HERE, on the GUI thread that owns the remembered settings;
        // the worker only ever sees the radio's own figure. No value = "leave
        // it": a node with no centre yet, or one this radio's converter cannot
        // deliver. The node's AIR centre may be below 0 Hz (see
        // pc::radioCentreSet) - whether it can be sent is airReachable's call.
        const std::optional<double> radioCentre =
            (pc::radioCentreSet(*n) && cascade::core::airReachable(conv, centre))
                ? std::optional<double>(cascade::core::radioFromAir(conv, centre))
                : std::nullopt;
        patchRadioPending_[id] = std::async(std::launch::async, [driver, args, label, rate,
                                                                 centre = radioCentre]() {
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
            // reason is kept - as is the reason for a rate the driver COERCED
            // on a call that succeeded (an RX888 in VHF mode, a Pluto above
            // its maximum), which through 0.99.34 was never read.
            const cascade::gui::RateSetOutcome set =
                cascade::gui::applySourceRate(*dev, rate, std::string());
            if (!set.ok) {
                r.error = "the radio refused " + std::to_string(rate / 1e6).substr(0, 5) +
                          " MS/s and runs at its own rate";
            } else if (!set.sourceError.empty()) {
                r.error = set.sourceError;
            }
            if (centre.has_value()) { dev->setCenterFrequencyHz(*centre); }
            // A patch radio has no gain slider of its own yet, so the radio's
            // own automatic gain is used where it has one - a dongle left at
            // its power-on gain hears very little.
            if (dev->autoGainSupported()) { dev->setAutoGain(true); }
            r.src = std::move(dev);
            return r;
        });
    }

    // --- the page's own scan, once the radios above are under way -------------
    // LAST, and that is the fix for the hardware check's finding: asked at the
    // top of this function on the page's first frame, the scan saw no radio
    // yet, went WHOLE-BUS, and the radios below then opened under it. Asked
    // here, a radio just started is pending, the plan defers, and the scan
    // runs once every radio is open - leaving their drivers out.
    if (patchScanWanted_ && !soapyScanPending_ &&
        soapyScanPlan().mode != cascade::gui::SoapyScanMode::Defer) {
        patchScanWanted_ = false;
        scanSoapy();
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
        cascade::core::diagLogf("patch: speaker node %u -> %s%s%s",
                                static_cast<unsigned>(sp.sink), destKind(dest.get()),
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
            const pc::Node* dn = patchGraph_.find(d->node);
            cascade::core::diagLogf("patch: decoder node %u (%s) started on radio node %u at "
                                    "%.0f S/s",
                                    static_cast<unsigned>(d->node),
                                    dn != nullptr ? dn->plugin.c_str() : "?",
                                    static_cast<unsigned>(id), d->rateHz);
        }
        for (const pc::NodeId r : set->refused) {
            const pc::Node* n = patchGraph_.find(r);
            cascade::core::diagLogf("patch: decoder node %u (%s) refused to start",
                                    static_cast<unsigned>(r),
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
    if (!patchRadios_.empty()) {
        // Worded apart, so a log says which: a patch stopped (STOP, ALL OFF,
        // the receiver view chosen) or the application closing.
        if (restoreMain) {
            cascade::core::diagLogf("patch: %zu patch radio(s) closing", patchRadios_.size());
        } else {
            cascade::core::diagLogf("patch: shutting down - %zu patch radio(s) closing",
                                    patchRadios_.size());
        }
    }
    for (auto& [id, radio] : patchRadios_) {
        (void)id;
        radio->stop();
    }
    // Destroying the radios destroys their runners and every set in them - on
    // this thread, after the readers have stopped - which finalises each
    // speaker's file and destroys each decoder handle where the ABI wants it.
    // ONE WAIT FOR EVERY SOUND CARD AMONG THEM: each card's close is handed to
    // its own thread as the radios go, and the batch then waits for all of
    // them against a single kCloseWaitMs deadline - five cards whose closes
    // hang cost one second here, not five.
    {
        cascade::source::SoundCardSource::CloseBatch closes;
        patchRadios_.clear();
    }
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
    // A SOUND CARD goes back through its own row: reopened on a worker AS IT
    // WAS RUNNING when the patch took it (keep.card) - not with whatever the
    // Source section's controls were edited to meanwhile. The patch radio
    // above has already been destroyed, and its close waited for, so the
    // card is free. Until it is back the config goes on naming it (the rule
    // below, for a card: cfg.soundCard names the card and both radio args
    // slots keep what they had); a successful open clears that
    // (pollSoundCard), a failed one leaves it.
    if (keep.kind == "soundcard") {
        if (!restoreKeep_.valid()) {
            restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(
                "soundcard", cfgSoapyArgs_, cfgNativeArgs_, std::string(), keep.rateHz);
            soundCardRemembered_ = keep.card;
            restoreKeepLabel_ = keep.label;
        }
        cascade::core::diagLogf("patch: handing %s back to the receiver", keep.label.c_str());
        sourceSel_ = kSoundCardRow;
        launchSoundCardOpen(false, keep.card);
        return;
    }
    // THE RADIO STAYS SAVED UNTIL IT IS BACK (0.99.36). patchMainKeep_ was the
    // only thing making the exit save name it, and it has just been cleared:
    // a hand-back that finds the radio unlisted, or whose open fails, used to
    // leave the config naming the generator. Remembered the way a startup
    // restore that could not open it is; a successful open clears it.
    if (!restoreKeep_.valid()) {
        cascade::gui::RememberedSource r;
        r.kind = keep.kind;
        if (keep.kind == "soapy") {
            r.soapyArgs = keep.args;
            r.nativeArgs = cfgNativeArgs_;
        } else {
            r.nativeArgs = keep.args;
            r.soapyArgs = cfgSoapyArgs_;
        }
        r.sampleRateHz = keep.rateHz;
        restoreKeep_ = r;
        restoreKeepLabel_ = keep.label;
    }
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
        std::string buf;
        cascade::core::formatUtf8(buf,
                      tr("the radio the patch page was using (%s) is not listed any more - "
                         "choose it again in Source"),
                      keep.label.c_str());
        sourceError_ = buf;
        cascade::core::diagWarnf("patch: could not hand %s back to the receiver - not listed",
                                 keep.label.c_str());
        return;
    }
    // Back where it was tuned when the patch took it: the AIR centre is handed
    // to the open itself, which sends it through THIS radio's converter. (It
    // used to be parked on the generator standing in and read back from
    // there, which lost a centre below 0 Hz on the air.)
    cascade::core::diagLogf("patch: handing %s back to the receiver", keep.label.c_str());
    selectSource(row, keep.centreHz);
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
        ImGui::SetTooltip(
            "%s", patchRunning_
                      ? tr("Stop the patch: every patch radio closes, recordings are\n"
                           "finished, and the receiver gets its radio back.")
                      : tr("Start the patch: every radio switched on opens. The receiver\n"
                           "hands its radio over and runs on the signal generator."));
    }

    // ALL OFF: larger, red, and always there. Every radio's switch goes off
    // and the patch stops.
    ImGui::SetCursorScreenPos(ImVec2(at.x + kR * 2.5f, at.y + kR * 0.25f));
    // A STOP control, so the stop button's roles (foxsdr-ui/1 stopBg and
    // stopText): today's rust and ivory exactly, and never Night Watch's
    // near-white "bad" as the face of a key that size.
    namespace th = cascade::gui::theme;
    ImGui::PushStyleColor(ImGuiCol_Button, th::toneHex(0xB8552F, 255, th::ink::StopBgBot));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, th::toneHex(0xE07A4E, 255, th::ink::StopBgTop));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, th::toneHex(0xE07A4E, 255, th::ink::StopBgTop));
    ImGui::PushStyleColor(ImGuiCol_Text, th::toneHex(0xEFE7D2, 255, th::ink::StopText));
    if (ImGui::Button(trId("ALL OFF###patchalloff"), ImVec2(150.0f, kR * 1.7f))) {
        patchAllOff();
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Switch every radio off and stop the patch. START switches\n"
                                   "them all on again."));
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
    std::string line;
    if (patchRunning_) {
        // Singular and plural as whole keys: an English "s" handed in by %s
        // is a word no catalogue can translate.
        cascade::core::formatUtf8(line,
                      radios == 1 ? tr("RUNNING - %zu of %zu radio open")
                                  : tr("RUNNING - %zu of %zu radios open"),
                      patchRadios_.size(), radios);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
    } else {
        cascade::core::formatUtf8(line,
                      radios == 1 ? tr("STOPPED - %zu of %zu radio switched on")
                                  : tr("STOPPED - %zu of %zu radios switched on"),
                      on, radios);
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
    }
    ImGui::TextUnformatted(line.c_str());
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
    if (ImGui::SmallButton(n.on ? trId("ON###radioOn") : trId("OFF###radioOn"))) {
        n.on = !n.on;
        patchUi_.dirty = true;
        cascade::core::diagLogf("patch: radio node %u switched %s", static_cast<unsigned>(n.id),
                                n.on ? "on" : "off");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            n.on ? tr("This radio is switched on. Press to close it alone - the rest\n"
                      "of the patch keeps running.")
                 : tr("This radio is switched off. Press to switch it on; it opens\n"
                      "while the patch is running."));
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
    ImGui::Text(tr("%zu of %zu inputs wired"), wired, pc::kMapInputs);
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
        if (ImGui::Button(trId("Fit to targets"), ImVec2(-FLT_MIN, 0.0f))) {
            view->second->requestFitToTracks();
        }
        if (ImGui::Button(trId("Whole world"), ImVec2(-FLT_MIN, 0.0f))) {
            view->second->requestWholeWorld();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped(
        tr("Wire the map output of up to %zu decoders here - aircraft from one radio, ships "
           "from another - and they share this map. Drag it to pan; zoom with the + and - "
           "keys in its corner, or the mouse wheel."),
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
    if (ImGui::Checkbox(trId("Squelch##sq"), &on)) {
        n.squelch = on;
        patchUi_.dirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            tr("Silences this demodulator's sound while the channel is quieter\n"
               "than the threshold - so a speaker or a recording hears signals,\n"
               "not the noise between them. Decoders are still given every sample."));
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
            ImGui::TextUnformatted(n.squelch ? tr("open") : tr("no squelch"));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
            ImGui::TextUnformatted(tr("closed"));
        }
        ImGui::PopStyleColor();
    }
}

void AppWindow::drawPatchRadioInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    const auto amber = cascade::gui::theme::vec(cascade::gui::theme::kAmber);

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Device"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::vector<PatchDeviceChoice> choices = patchDeviceChoices();
    if (ImGui::BeginCombo("##patchdevice", patchDeviceLabel(n.device).c_str())) {
        // OPENING THE LIST IS ASKING FOR IT (0.99.40), as the Source section's
        // first open is: the view shown at start-up asked for nothing, so the
        // SoapySDR scan and the sound card listing are started here, once.
        if (!patchListsWanted_) {
            patchListsWanted_ = true;
            if (!soapyScanned_ || soapyScanPartial_) { patchScanWanted_ = true; }
        }
        // The recordings are read afresh each time the list opens - a file
        // saved a moment ago is in it - and on no other frame. The rows drawn
        // this frame are the ones read last time; the new list is there on
        // the next frame, which is the one the eye reaches it on.
        if (ImGui::IsWindowAppearing()) { patchListRecordings(); }
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
            if (holder != nullptr) {
                std::string buf;
                cascade::core::formatUtf8(buf, tr("  (used by %s)"), holder->name.c_str());
                text += buf;
            }
            // IN USE BY THE RECEIVER IS NOT "NOT AVAILABLE" (2026-09-23): the
            // receiver lends its radio to the patch when the patch starts, so
            // the row is offered, and says what will happen to it.
            else if (device_ != nullptr &&
                     pc::sameDevice(c.key, pc::makeDeviceKey(sourceKind_, deviceArgs_))) {
                text += tr("  (the receiver's - lent to the patch when it starts)");
            }
            ImGui::BeginDisabled(holder != nullptr);
            // Not trId(): text is composed at runtime (a device's own label
            // plus an already-translated suffix), not a fixed English key, and
            // this Selectable's id is already made unique by the PushID(i)
            // above rather than by its label text.
            if (ImGui::Selectable(text.c_str(), c.key == n.device)) {
                n.device = c.key;
                // A recording's rate is its own: the node takes it now, so
                // the patch is planned at the rate it will run at.
                for (const pc::RecordingInfo& r : patchRecordings_) {
                    if (r.key == c.key) { n.rateHz = r.rateHz; }
                }
                patchUi_.dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // LOOK AGAIN from here, rather than sending the user to the Source panel:
    // a radio plugged in after the page opened, or one a scan beside an open
    // radio had to leave out, is one press away (2026-09-23).
    ImGui::BeginDisabled(soapyScanPending_);
    if (ImGui::SmallButton(trId("Look for radios"))) {
        patchListsWanted_ = true;   // the sound cards too
        scanNative();
        scanSoapy();
        patchListRecordings();
    }
    ImGui::EndDisabled();
    if (soapyScanPending_) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted(tr("looking..."));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Centre (MHz)"));
    double mhz = n.freqHz / 1e6;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputDouble("##patchcentre", &mhz, 0.1, 1.0, "%.6f",
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        setPatchRadioCentre(n, mhz * 1e6);
    }
    drawPatchCentreNote(n);

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Sample rate"));
    char rateText[32];
    std::snprintf(rateText, sizeof(rateText), "%.3f MS/s", radioRate(n) / 1e6);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (pc::deviceSetsItsOwnRate(n.device)) {
        // A RECORDING'S RATE IS NOT A SETTING: it is the rate it was made at,
        // and IqFileSource refuses any other. Shown, and said, not offered.
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::TextUnformatted(recordingRateText(radioRate(n)).c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextWrapped("%s", tr("The recording sets the rate."));
        ImGui::PopStyleColor();
    } else if (ImGui::BeginCombo("##patchrate", rateText)) {
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
        ImGui::Text(tr("running %.3f MS/s"), run->second->rateHz() / 1e6);
        ImGui::Text(tr("at %.6f MHz"), run->second->centreHz() / 1e6);
        ImGui::PopStyleColor();
    } else if (patchRadioPending_.count(n.id) != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, muted);
        ImGui::TextUnformatted(tr("opening the device..."));
        ImGui::PopStyleColor();
    }
    const auto err = patchRadioError_.find(n.id);
    if (err != patchRadioError_.end()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::bad());
        ImGui::TextWrapped("%s", err->second.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped(
        tr("Up to %zu radios, each its own device. While this page is open the receiver runs "
           "on the signal generator; closing it gives the radio back."),
        pc::kMaxRadios);
    ImGui::PopStyleColor();
}

void AppWindow::drawPatchSinkInspector(pc::Node& n) {
    const auto muted = cascade::gui::theme::vec(cascade::gui::theme::kInkMuted);
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Sound goes to"));
    const std::string key = n.device.empty() ? std::string("wav") : n.device;
    std::string current;
    switch (pc::outputKind(key)) {
        case pc::OutputKind::Wav: current = tr("A WAV file"); break;
        case pc::OutputKind::Mp3: current = tr("An MP3 file"); break;
        case pc::OutputKind::Speakers: current = tr("The speakers"); break;
        case pc::OutputKind::Device: current = pc::outputDeviceName(key); break;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##patchoutput", current.c_str())) {
        if (ImGui::Selectable(trId("A WAV file"), pc::outputKind(key) == pc::OutputKind::Wav)) {
            n.device = "wav";
            patchUi_.dirty = true;
        }
        const bool mp3 = cascade::core::Mp3Writer::available();
        ImGui::BeginDisabled(!mp3);
        if (ImGui::Selectable(mp3 ? trId("An MP3 file") : trId("An MP3 file (needs Windows)"),
                              pc::outputKind(key) == pc::OutputKind::Mp3)) {
            n.device = "mp3";
            patchUi_.dirty = true;
        }
        ImGui::EndDisabled();
        if (ImGui::Selectable(trId("The speakers"),
                              pc::outputKind(key) == pc::OutputKind::Speakers)) {
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
        ImGui::TextWrapped(tr("Files go in %s, one per speaker, named after it."),
                           recordDir_.c_str());
    }
    ImGui::PopStyleColor();
}

}  // namespace cascade::gui
