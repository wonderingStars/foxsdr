// engine.cpp - the Engine's machinery from gui/app_window.cpp, moved VERBATIM (engine
// extraction stage 3a, docs/engine-stage3.md): each definition is the
// window's, renamed AppWindow:: -> Engine::, with the few lines that reached
// into the window turned into calls on the host (engine_host.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include "core/patch_io.hpp"
#include "core/patch_levels.hpp"
#include "core/patch_plan.hpp"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include "core/version.hpp"
#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/freq_import.hpp"
#include "core/diag_report.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "core/image_write.hpp"
#include "core/package_identity.hpp"
#include "engine/rate_follow_status.hpp"
#include "engine/soundcard_panel.hpp"
#include "dsp/window.hpp"
#include "engine/running_view.hpp"
#include "net/control_ops.hpp"
#include "net/status_compose.hpp"
#include "engine/tune_control.hpp"
#include "source/iq_file_source.hpp"
#include "source/rsp_rows.hpp"
#include "source/soapy_enum_proc.hpp"
#include "engine/device_scan_plan.hpp"
#include "usb/usb_device.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/receiver_tables.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tx_frequency.hpp"


namespace cascade::engine {

// The helpers moved out of src/gui keep their namespace (cascade::gui), so
// the code that moved with them reads exactly as it did.
using namespace cascade::gui;
using cascade::i18n::tr;
using cascade::i18n::trId;
using cascade::gui::kBwCount;
using cascade::gui::kBwHz;
using cascade::gui::bandwidthStepIndex;
using cascade::gui::formatBandwidth;

// The Pipeline is built exactly as the window built it: the same rate, FFT
// length and smoothing (engine/receiver_tables.hpp), audio on.
Engine::Engine() : Engine(ownHost_) {}

Engine::Engine(EngineHost& host)
    : host_(host),
      pipeline_(cascade::core::Pipeline::Config{kSampleRateHz, kFftSize, kAveragingAlpha,
                                                /*audioEnabled=*/true}) {}

Engine::~Engine() = default;

namespace {

// The WatchdogPause the window's plugin rescan held (core/hang_watchdog.hpp),
// through the host: paused now, resumed on every way out of the scope.
class HostWatchdogPause {
public:
    explicit HostWatchdogPause(EngineHost& h) : h_(h) { h_.pauseWatchdog(); }
    ~HostWatchdogPause() { h_.resumeWatchdog(); }
    HostWatchdogPause(const HostWatchdogPause&) = delete;
    HostWatchdogPause& operator=(const HostWatchdogPause&) = delete;

private:
    EngineHost& h_;
};

}  // namespace

namespace {

// Takes a resolved device-open result and lets it go, which is precisely what
// closes the device: the result owns the SoapySource and its destructor is the
// close. Templated only so it can live here, at file scope, without naming
// AppWindow's private result type.
template <typename Fut>
void drainDeviceOpen(Fut& f) {
    try {
        auto r = f.get();
        (void)r;  // destroyed here; ~SoapySource releases the handle
    } catch (...) {
        // A worker that threw has no device to release. Never propagate: this
        // runs during teardown, and on the detached path with no catcher above
        // it at all.
    }
}

// The scan's equivalent, and it is deliberately NOT drainDeviceOpen. That one
// exists to CLOSE a device — the result owns the SoapySource. A scan result
// owns nothing but an enumeration list of strings, so there is no handle at
// stake and this drain has exactly one job: let the future's blocking
// destructor run somewhere that is not the GUI thread.
template <typename Fut>
void drainSoapyScan(Fut& f) {
    try {
        auto r = f.get();
        (void)r;  // just an enumeration list; dropped here
    } catch (...) {
        // Never propagate: this runs during teardown, and on the detached path
        // with no catcher above it at all.
    }
}

constexpr float kSoapyGainDefaultDb = 30.0f;

// Reads a catalogue index from the LOCAL filesystem and parses it with the
// same parseIndex() the network path uses. See AppConfig::pluginCatalogueUrl
// for why this form exists: a catalogue on a corporate share, and the only
// way the success path of this UI can be exercised without a live server.
//
// It grants nothing the network path does not already allow. parseIndex still
// refuses every non-https download URL and every malformed sha256, and
// install() re-checks both before it opens a socket — so the worst a hostile
// local index can do is offer an entry that install() then refuses.
// The same kMaxIndexBytes cap applies, because "it is on our own disk" is not
// a reason to read a 4 GB document into memory.
bool readLocalCatalogue(const std::string& path,
                        std::vector<cascade::core::PluginCatalogEntry>& out,
                        std::string& error) {
    out.clear();
    error.clear();
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) {
        error = "cannot read the catalogue file \"" + path + "\": " + ec.message();
        return false;
    }
    if (size > cascade::core::PluginRepo::kMaxIndexBytes) {
        error = "the catalogue file \"" + path + "\" is larger than the " +
                std::to_string(cascade::core::PluginRepo::kMaxIndexBytes) + "-byte limit";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open the catalogue file \"" + path + "\"";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    return cascade::core::PluginRepo::parseIndex(text, out, error);
}

}  // namespace

void Engine::refreshDiagContext() {
    cascade::core::DiagContext ctx;
    ctx.version = cascade::versionString();
    ctx.commit = cascade::gitCommit();
    ctx.os = cascade::core::osDescription();
    ctx.arch = cascade::core::archDescription();
    ctx.mode = kModeNames[modeIndex_];
    ctx.sourceKind = sourceKind_;
    ctx.sampleRateHz = pipeline_.activeSource().sampleRateHz();
    // ANY RADIO, not just a Soapy one: what this flag tells a report reader
    // is whether third-party or USB driver code was live in the process, and
    // a native driver is as much a device open as a vendor module is.
    ctx.deviceOpen = (device_ != nullptr);
    // MODEL ONLY - sanitiseDevice strips the serial, exactly as the usage
    // report does. A report is a support artefact, not a hardware fingerprint.
    ctx.sdrModel = deviceModel_;
    std::size_t loaded = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        ++loaded;
        if (ctx.plugins.size() < 32) { ctx.plugins.push_back(p.name + " " + p.version); }
    }
    cascade::core::setDiagContext(ctx);

    // The module table only has to be rebuilt when something LOADED CODE.
    // Doing it every frame would mean walking the loader's module list 60
    // times a second for a table that changes a handful of times a session.
    // This covers the plugins; the OTHER thing that maps code into this
    // process after start-up is a device open, and that has its own refresh
    // where the open completes (pollSourceAsync) - a device open changes no
    // plugin count, so this test cannot see it.
    if (loaded != diagPluginCount_) {
        diagPluginCount_ = loaded;
        cascade::core::refreshModuleTable();
    }
}

void Engine::pollAudioHealth() {
    cascade::sink::AudioOut& out = pipeline_.audio();
    // Never opened means there is no device on this machine (or audio is
    // configured off). That is a steady state, not a fault, and retrying it
    // once a second forever would be noise.
    if (!out.everOpened()) { return; }

    // Ring low-water sampling happens EVERY frame — not gated behind the 1 Hz
    // probe below. At 48 kHz a ring can dip toward empty and refill well
    // inside a second, and a mark that was only ever read once a second would
    // miss most of what it exists to catch.
    const std::size_t ringFrames = out.ringFrames();
    if (ringFrames < audioRingLowWaterFrames_) { audioRingLowWaterFrames_ = ringFrames; }

    const double now = host_.frameTimeS();

    // Once-a-minute starvation digest. Silent unless something actually
    // starved in the window just closed — same "quiet unless it has news"
    // posture as the recorder's own status lines, and the only way a number
    // nobody watches (this goes to the log, not the screen) stays worth
    // reading when it does fire.
    if (now - lastAudioLogSec_ >= 60.0) {
        const std::uint64_t underrunsNow = out.underruns();
        const std::uint64_t primingNow = out.primingCallbacks();
        if (underrunsNow > audioUnderrunsAtLogStart_) {
            const double rateHz = cascade::core::Pipeline::kAudioRateHz;
            const double lowWaterMs =
                1000.0 * static_cast<double>(audioRingLowWaterFrames_) / rateHz;
            const double capacityMs =
                1000.0 * static_cast<double>(out.ringCapacityFrames()) / rateHz;
            cascade::core::diagLogf(
                "audio: %llu starved callbacks in the last minute (%llu priming), "
                "ring low water %.0f ms of %.0f ms",
                static_cast<unsigned long long>(underrunsNow - audioUnderrunsAtLogStart_),
                static_cast<unsigned long long>(primingNow - audioPrimingAtLogStart_),
                lowWaterMs, capacityMs);
        }
        audioUnderrunsAtLogStart_ = underrunsNow;
        audioPrimingAtLogStart_ = primingNow;
        audioRingLowWaterFrames_ = ringFrames;  // next minute's mark starts here
        lastAudioLogSec_ = now;
    }

    // 1 Hz. Fast enough that a dropout is a hiccup rather than an outage,
    // slow enough that a genuinely absent device is not hammered with open
    // attempts. ImGui's clock is the frame clock, which is what "once per
    // second of running UI" should mean here.
    if (now - lastAudioProbeSec_ < 1.0) { return; }
    lastAudioProbeSec_ = now;

    // AN OPEN IN FLIGHT ALREADY OWNS THE SINK. Asking a sink that a worker is
    // inside open() on whether its stream is alive is a race, and acting on
    // the answer would start a SECOND concurrent device open — which is the
    // shape of fault this whole path was written to end, not to create. The
    // frame after the open completes asks the question again.
    if (audioOpen_.inFlight()) { return; }

    if (out.streamAlive()) { return; }

    // The stream is dead. Re-enumerate before choosing a target: the device
    // list is how recoveryDeviceIndex() resolves the remembered NAME to a
    // current index, and the reason it is done by name is that this list can
    // renumber between the open and now.
    devices_ = out.listOutputDevices();
    const int target = cascade::sink::recoveryDeviceIndex(
        out.openedDeviceRequested(), out.openedDeviceName(), devices_);

    // OFF THIS THREAD. A dead stream is usually a device that has gone away,
    // and a device that has gone away is exactly the one whose open blocks —
    // so this is the LAST place in the application that may call a blocking
    // open on the frame loop, and it used to do it once a second for as long
    // as the condition lasted. requestAudioOpen answers false when the device
    // has not replied inside the bound; the result lands in pollAudioOpen when
    // it does, and this tick goes back to rendering.
    (void)requestAudioOpen(target, true);
}

bool Engine::requestAudioOpen(int deviceIndex, bool recovery) {
    const cascade::gui::AudioOpen::Outcome outcome = audioOpen_.request(
        deviceIndex, recovery ? kAudioOpenByWatchdog : kAudioOpenByUser);
    if (outcome != cascade::gui::AudioOpen::Outcome::Finished) {
        // Still inside the driver. Said in the panel rather than left to look
        // like nothing happened — "I picked the device and nothing changed" is
        // how a silent wait reads, and it is the reading that sends the next
        // report to the wrong end of the chain.
        audioHealthNote_ = tr(kAudioBusyNote);
        return false;
    }
    applyAudioOpenResult();
    return audioOpen_.result().ok;
}

void Engine::pollAudioOpen() {
    if (!audioOpen_.poll()) { return; }
    applyAudioOpenResult();
}

void Engine::pollMicOpen() {
    if (!micOpen_.poll()) { return; }
    // The "still opening" line goes; a refusal says so in its place.
    if (transmitError_ == kAudioBusyNote) { transmitError_.clear(); }
    if (!micOpen_.result().ok) { transmitError_ = FOX_TR_NOOP("no microphone could be opened"); }
}

void Engine::applyAudioOpenResult() {
    const cascade::gui::AudioOpen::Result r = audioOpen_.result();
    // THE DSP THREAD'S MIRROR, published here rather than by the worker: it
    // lives under Pipeline's audioMutex_ and belongs to the Pipeline, which an
    // abandoned worker may outlive. See Pipeline::audioOpener().
    pipeline_.publishAudioChannels(r.ok);

    // Re-enumerate now that the sink is this thread's again: the list is how
    // the row below is resolved, and the reason a stream died is usually that
    // a device went away — so it can be SHORTER than the one deviceIndex_ was
    // chosen against.
    devices_ = pipeline_.audio().listOutputDevices();

    if (!r.ok) {
        // Say so rather than failing silently — silent failure is the exact
        // bug this whole path exists to end. The next tick tries again.
        audioHealthNote_ = tr("audio output stopped and could not be reopened");
        // The Sinks combo subscripts deviceIndex_ guarded only by "not empty",
        // so leaving a stale row against a shorter list is an out-of-bounds
        // read on the very next frame.
        deviceIndex_ = cascade::sink::clampDeviceRow(deviceIndex_, devices_);
        return;
    }

    // Keep the Sinks combo honest about what is actually open. Without this
    // the panel would name the old device while audio came out of another.
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (devices_[static_cast<std::size_t>(i)].index == r.deviceIndex) {
            deviceIndex_ = i;
            break;
        }
    }
    if (r.deviceIndex < 0) {
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
            if (devices_[static_cast<std::size_t>(i)].isDefault) { deviceIndex_ = i; }
        }
    }
    // Neither loop above is guaranteed to assign: a target that opened but is
    // not in the list we just enumerated (it renumbered again between the two
    // calls) leaves the old row standing against the new list. Same
    // out-of-bounds, so the same clamp closes it here too.
    deviceIndex_ = cascade::sink::clampDeviceRow(deviceIndex_, devices_);

    if (r.tag != kAudioOpenByWatchdog) {
        // The user picked this device themselves and can see the result. Only
        // the "still opening" line this request may have put up is taken down:
        // a recovery note from earlier in the session is the watchdog's, and
        // stays, because "audio stopped and came back on its own" is the thing
        // a user would otherwise report as a fault in their radio.
        if (audioHealthNote_ == tr(kAudioBusyNote)) { audioHealthNote_.clear(); }
        return;
    }
    ++audioRecoveries_;
    std::string buf;
    // Two whole sentences rather than an "s" glued on: a translation's plural
    // is not a suffix.
    cascade::core::formatUtf8(buf,
                  audioRecoveries_ == 1
                      ? tr("output device stopped and was restarted (%d time)")
                      : tr("output device stopped and was restarted (%d times)"),
                  audioRecoveries_);
    audioHealthNote_ = buf;
}

bool Engine::soapyScanGated() const {
    // THREE WAYS A RADIO IS OPEN, and the third is the one worth explaining.
    // device_ is the device installed in the pipeline; deviceOpenPending_ is
    // one being made on its worker right now, Device::make already inside the
    // driver stack. anyDeviceOpen() is the process-wide count, which by design
    // never comes down for a device the dead-device policy abandoned: the
    // module still owns that radio, and in the wedged case one of our own
    // threads is still executing inside it, and in the faulted case the
    // driver's own reader thread may be. A probe that resets that dongle from
    // outside faults a thread the guard cannot reach. So an abandoned radio
    // keeps the scan gated for the rest of the session, which is the same
    // bargain the user is given in words: restart FoxSDR to use it again. The
    // rows already scanned stay usable, and the automatic reopen
    // (pollSoapyRecovery) needs no scan at all - it has the args.
    //
    // A NATIVE RADIO DOES NOT GATE THE SOAPY SCAN, and this is the one line
    // of the rule that changed in 0.91.0. The gate exists because the vendor
    // probe opens and resets every dongle on the bus - including one this
    // process is streaming from - and that is as true of a dongle we are
    // holding natively as of one held through a module. So soapyView_ is NOT
    // what is asked here: device_ is, whatever kind it is. The three
    // conditions are unchanged in meaning; only the type of the first has
    // widened.
    //
    // AND THE PATCH PAGE'S RADIOS (2026-09-23). A patch runs up to five radios
    // of its own, natively or through SoapySDR, and a native patch dongle was
    // invisible to this gate: a whole-bus scan could have probed it
    // mid-stream exactly as the 0.90.0 report describes.
    return device_ != nullptr || deviceOpenPending_ ||
           cascade::source::SoapySource::anyDeviceOpen() || !patchRadios_.empty() ||
           !patchRadioPending_.empty();
}

cascade::gui::SoapyScanPlan Engine::soapyScanPlan() const {
    // Every radio this process has open: the receiver's and each patch radio's,
    // named by family the way both name them. A patch radio on the generator
    // is not a radio. See engine/device_scan_plan.hpp for the rule.
    std::vector<cascade::gui::OpenRadio> open;
    int soapyListed = 0;
    if (device_ != nullptr) {
        open.push_back({sourceKind_, deviceArgs_});
        if (sourceKind_ == "soapy") { ++soapyListed; }
    }
    for (const auto& entry : patchRadios_) {
        const cascade::core::patch::Node* n = patchGraph_.find(entry.first);
        if (n == nullptr) {
            // A running radio whose node is gone: its family cannot be named,
            // so the plan defers - never a guess.
            open.push_back({"unknown", std::string()});
            continue;
        }
        if (cascade::core::patch::isGeneratorKey(n->device)) { continue; }
        const std::string kind = cascade::core::patch::deviceDriver(n->device);
        // A sound card is not on the bus a SoapySDR probe walks, so it has
        // nothing to protect from one (gui::scanMayProbe says the same).
        if (kind == "soundcard") { continue; }
        open.push_back({kind, cascade::core::patch::deviceArgs(n->device)});
        if (kind == "soapy") { ++soapyListed; }
    }
    const int unaccounted =
        std::max(0, cascade::source::SoapySource::openDeviceCount() - soapyListed);
    const bool opening = deviceOpenPending_ || !patchRadioPending_.empty();
    return cascade::gui::planSoapyScan(open, unaccounted, opening);
}

std::string Engine::soapyScanGateDevice() const {
    // The MODEL, never the raw args: this string goes into the diagnostic
    // log, and the args carry the serial (see every other sanitiseDevice
    // site). deviceModel_ is that model for either family - a native row's
    // args are nothing BUT a serial, so sanitiseDevice would answer "".
    if (device_ != nullptr && !deviceModel_.empty()) { return deviceModel_; }
    if (deviceOpenPending_ && !deviceBusyLabel_.empty()) { return deviceBusyLabel_; }
    // A patch radio, by the name the user gave its part - never its args.
    for (const auto& entry : patchRadios_) {
        if (const cascade::core::patch::Node* n = patchGraph_.find(entry.first)) {
            return "the patch radio '" + n->name + "'";
        }
    }
    if (!patchRadioPending_.empty()) { return "a patch radio"; }
    return "a radio this session could not release";
}

void Engine::scanSoapy() {
    // Kick the enumeration onto a worker and return immediately - see the
    // header for why this may not run inline. One at a time: a second scan
    // while one is in flight would race the result into soapyDevices_.
    //
    // THREE ANSWERS since 2026-09-23 (engine/device_scan_plan.hpp):
    //
    //  - nothing open: the whole-bus scan, exactly as before;
    //  - radios open, every one of a known family: a scan that leaves THEIR
    //    drivers out and asks every other driver on its own - so a B200 is
    //    found beside a streaming RTL-SDR, which the owner could not do;
    //  - anything it cannot vouch for (a radio still opening, a family it
    //    does not know, a SoapySDR device nobody accounts for): DEFERRED, as
    //    in 0.90.1 - gui::deviceScanAllowed has the field report. A deferral
    //    leaves soapyScanned_ FALSE, or the section would never scan once the
    //    radio closes, and says why ONCE, because the combo asks every frame.
    //
    // Either way the receiver's open SoapySDR device keeps a row, so a
    // session that has not scanned yet still shows it rather than a blank.
    const auto keepOpenRow = [this]() {
        // A NATIVE RADIO NEEDS NOTHING HERE: scanNative() is never deferred,
        // so nativeDevices_ always holds its row and sourceSel_ already
        // points at it. Only a Soapy device can be open with no scan behind
        // it (restored from the config, or opened before the gate closed).
        if (soapyView_ == nullptr || deviceArgs_.empty()) { return; }
        bool listed = false;
        for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
            if (soapyDevices_[i].args == deviceArgs_) {
                listed = true;
                sourceSel_ = soapyRowBase() + static_cast<int>(i);
            }
        }
        if (!listed) {
            // The row a scan would have produced, from what the open
            // itself told us: the live name less its "SoapySDR: " prefix
            // is the label a vendor module gives its device, and the args
            // are the exact string that reopens it. A later real scan
            // replaces the whole list and re-finds the device by args.
            cascade::source::SoapyDeviceInfo row;
            row.label = pipeline_.activeSource().name();
            const std::size_t colon = row.label.rfind(": ");
            if (colon != std::string::npos) { row.label = row.label.substr(colon + 2); }
            row.args = deviceArgs_;
            soapyDevices_.push_back(std::move(row));
            sourceSel_ = soapyRowBase() + static_cast<int>(soapyDevices_.size() - 1);
        }
    };

    if (soapyScanPending_) { return; }
    const bool whole = cascade::gui::deviceScanAllowed(soapyScanGated(), soapyScanPending_,
                                                       deviceOpenPending_);
    const cascade::gui::SoapyScanPlan plan = soapyScanPlan();
    if (!whole && plan.mode != cascade::gui::SoapyScanMode::SkipSome) {
        if (soapyScanGated() && !soapyScanDeferredLogged_) {
            soapyScanDeferredLogged_ = true;
            cascade::core::diagLogf(
                "soapy: device scan deferred while %s is open - the vendor probe opens "
                "and resets every dongle it finds (close the radio to look for other "
                "devices)",
                soapyScanGateDevice().c_str());
        }
        keepOpenRow();
        return;
    }
    std::vector<std::string> skip;
    if (!whole) {
        keepOpenRow();
        skip = plan.skipDrivers;
        std::string names;
        for (const std::string& d : skip) { names += (names.empty() ? "" : ", ") + d; }
        cascade::core::diagLogf(
            "soapy: scanning beside %s - every SoapySDR driver is asked except %s, whose "
            "probe would reset the open radio",
            soapyScanGateDevice().c_str(), names.c_str());
    }
    soapyScanned_ = true;  // claimed now so the combo does not re-request
    soapyScanPending_ = true;
    soapyScanSkip_ = skip;
    soapyScanPartial_ = !whole;
    // UHD IS ASKED ONLY WHEN A USRP COULD BE HERE (F204602B5329B268, 0.99.35:
    // its probe killed the scan's child on a machine whose only radio was an
    // SDRplay). The Soapy args this session knows of - the file's, the last
    // Soapy radio's, the open one's and every patch radio's - say whether the
    // user has one; the USB listing, taken on the scan's own thread below
    // because SetupAPI is not free, says whether one is plugged in. See
    // gui::soapyDriversWithNoHardware.
    std::vector<std::string> namedSoapyArgs = {startupSoapyArgs_, cfgSoapyArgs_};
    if (sourceKind_ == "soapy") { namedSoapyArgs.push_back(deviceArgs_); }
    for (const cascade::core::patch::Node& n : patchGraph_.nodes()) {
        if (cascade::core::patch::deviceDriver(n.device) == "soapy") {
            namedSoapyArgs.push_back(cascade::core::patch::deviceArgs(n.device));
        }
    }
    const bool lookForNetworkUsrps = lookForNetworkUsrps_;
    soapyScanAbsent_ = std::make_shared<std::vector<std::string>>();
    const std::shared_ptr<std::vector<std::string>> absentOut = soapyScanAbsent_;
    // enumerate() never throws and is simply empty on a machine with no
    // vendor modules; this is also the hot-plug refresh path.
    const auto scanHook = testHooks_.soapyScan;  // null in every build of the application
    soapyScanFuture_ = std::async(std::launch::async, [skip, namedSoapyArgs,
                                                       lookForNetworkUsrps, absentOut, scanHook] {
        if (scanHook != nullptr) { return scanHook(); }
        std::vector<cascade::usb::UsbId> present;
        const bool listed = cascade::usb::presentUsbIds(present);
        std::vector<cascade::gui::UsbVidPid> ids;
        for (const cascade::usb::UsbId& id : present) { ids.push_back({id.vid, id.pid}); }
        const std::vector<std::string> absent = cascade::gui::soapyDriversWithNoHardware(
            ids, listed, namedSoapyArgs, lookForNetworkUsrps);
        // Handed back for the hint under the Source list; the future being
        // ready is what makes it safe to read there.
        *absentOut = absent;
        return (skip.empty() && absent.empty())
                   ? cascade::source::SoapySource::enumerate()
                   : cascade::source::SoapySource::enumerate(skip, absent);
    });
}

void Engine::reapPendingDeviceOpen() {
    if (!deviceOpenPending_ || !deviceOpenFuture_.valid()) { return; }

    // WHY THIS EXISTS. std::future's destructor for a std::async(launch::async)
    // task BLOCKS until the worker returns, and deviceOpenFuture_ is a member,
    // so quitting while a device open was in flight parked the GUI thread
    // inside ~AppWindow for the whole of SoapySDR::Device::make() — seconds on
    // a healthy B200, and unbounded against a device that is wedged or has
    // been unplugged mid-open. The window is already gone by then, so it
    // presents as the application hanging after it closed.
    //
    // Grace period first: an open that is already finished (or a few
    // milliseconds from it) is reaped right here, which keeps the common case
    // free of an extra thread and of the process-exit race below. 250 ms is
    // short enough not to be felt and long enough to cover every open that was
    // not actually stuck.
    constexpr auto kQuitGrace = std::chrono::milliseconds(250);
    if (deviceOpenFuture_.wait_for(kQuitGrace) == std::future_status::ready) {
        drainDeviceOpen(deviceOpenFuture_);
        deviceOpenPending_ = false;
        return;
    }

    // Still inside make(). THE CONSERVATIVE CHOICE, stated explicitly because
    // it is a trade and not a free win: the pending work is moved onto a
    // detached reaper so quit stays responsive, and the reaper's only job is
    // to take the result and DESTROY it — DeviceOpenResult owns the
    // SoapySource, whose destructor closes the device, so an open that
    // completes after quit still releases its handle rather than leaking it
    // for the lifetime of the process.
    //
    // What it does not promise: if the process exits before the reaper
    // finishes, Windows terminates that thread wherever it happens to be and
    // the handle is released by the operating system with the process instead.
    // That is the accepted cost — the alternative on offer is the hang above,
    // and no amount of waiting can bound a driver call that is not going to
    // return.
    std::thread([f = std::move(deviceOpenFuture_)]() mutable {
        drainDeviceOpen(f);
    }).detach();
    deviceOpenPending_ = false;
}

void Engine::reapPendingSoapyScan() {
    if (!soapyScanPending_ || !soapyScanFuture_.valid()) { return; }

    // THE SAME BLOCKING DESTRUCTOR AS reapPendingDeviceOpen, on the other
    // future. std::async(launch::async) futures block in ~future until the
    // worker returns, and soapyScanFuture_ is a member, so quitting while the
    // lazy scan was in flight parked the GUI thread inside ~AppWindow for the
    // whole of SoapySDR::Device::enumerate(). That call walks every registered
    // vendor module's discovery routine — it is multi-second on a healthy
    // machine with several drivers installed and unbounded against a wedged
    // one — and the window is already gone by then, so it presents as the
    // application hanging after it closed. The scan is started lazily the
    // first time the source combo is opened, which is a moment away from the
    // user deciding there is no radio here and quitting.
    //
    // Grace period first, for the same reason: a scan a few milliseconds from
    // finishing is reaped right here, keeping the common case free of an extra
    // thread and of the process-exit race below.
    constexpr auto kQuitGrace = std::chrono::milliseconds(250);
    if (soapyScanFuture_.wait_for(kQuitGrace) == std::future_status::ready) {
        drainSoapyScan(soapyScanFuture_);
        soapyScanPending_ = false;
        return;
    }

    // Still inside enumerate(). Moved onto a detached reaper so quit stays
    // responsive. WHERE THIS DIFFERS FROM THE OPEN REAPER, and it is worth
    // being explicit because the two look identical: the open reaper has a
    // real duty after quit — its result owns the device handle, and dropping
    // it is what closes the radio. A scan result is an enumeration list and
    // owns no handle at all, so this reaper releases nothing; it exists purely
    // so the wait happens off the GUI thread. If the process exits first the
    // thread is terminated wherever it stands and nothing is left behind that
    // the operating system would not have reclaimed anyway.
    std::thread([f = std::move(soapyScanFuture_)]() mutable {
        drainSoapyScan(f);
    }).detach();
    soapyScanPending_ = false;
}

void Engine::launchDeviceOpen(DeviceOpenResult r, const std::string& busyLabel) {
    deviceBusyLabel_ = busyLabel;
    deviceOpenPending_ = true;
    // Stamp the request with the selection it belongs to. Nothing is installed
    // yet, so the counter is NOT bumped here — only the answer's right to be
    // applied is recorded.
    deviceOpenReqGen_ = sourceGen_;
    // The RSP pre-Init tune (see the worker below) is a RADIO frequency, so
    // it is converted here, on the GUI thread that owns the converter table,
    // for the radio this request names. Only a frequency the radio can be
    // told (above 0 Hz, and deliverable through its converter) is written.
    r.preTuneRadioHz.reset();
    if (r.kind == "sdrplay" && r.keepCenterHz.has_value()) {
        const cascade::core::ConverterSetting conv =
            converterForKey(cascade::core::converterRadioKey(r.kind, r.args));
        if (cascade::core::airReachable(conv, *r.keepCenterHz)) {
            const double radioHz = cascade::core::radioFromAir(conv, *r.keepCenterHz);
            if (radioHz > 0.0) { r.preTuneRadioHz = radioHz; }
        }
    }
    // The open runs on a worker: Device::make() is the multi-second, USB-bus
    // -walking call that used to freeze the GUI here. The request travels
    // into the worker and comes back as the answer, with the device (or the
    // reason) filled in.
    deviceOpenFuture_ = std::async(std::launch::async, [r = std::move(r)]() mutable {
        std::unique_ptr<cascade::source::DeviceSource> dev = makeDeviceSource(r.kind);
        if (!dev) {
            r.error = "\"" + r.kind + "\" is not a source kind this build can open";
            return std::move(r);
        }
        if (!dev->open(r.args)) {
            r.error = dev->lastError();
            // THE ONE FALLBACK, and it is a real dongle rather than a
            // hypothetical: an E4000 or FC0012/13 tuner, which the native
            // RTL-SDR driver does not support and says so in as many words
            // (gui::nativeOpenShouldFallBack matches that sentence). A user
            // on one of those was reaching their radio perfectly well through
            // SoapySDR before this release, and the prefer-native rule must
            // not be what takes it away from them. So the Soapy args the
            // decision was made FROM are carried along and opened instead.
            if (!r.fallbackSoapyArgs.empty() &&
                cascade::gui::nativeOpenShouldFallBack(r.error)) {
                cascade::core::diagWarnf(
                    "source: the native %s driver refused this radio (%s); opening it "
                    "through SoapySDR instead",
                    r.kind.c_str(), r.error.c_str());
                // Through makeDeviceSource like every other construction, so
                // "soapy" means one thing everywhere (and the test seam's
                // fake stands in here too).
                std::unique_ptr<cascade::source::DeviceSource> soapy = makeDeviceSource("soapy");
                if (soapy && soapy->open(r.fallbackSoapyArgs)) {
                    // The radio the user chose, under its native key, so its
                    // converter follows it onto the Soapy one.
                    r.fellBackFromKey = cascade::core::converterRadioKey(r.kind, r.args);
                    r.kind = "soapy";
                    r.args = r.fallbackSoapyArgs;
                    r.error.clear();
                    dev = std::move(soapy);
                } else {
                    // Both refused it. The NATIVE reason is the one kept: it
                    // is the specific one ("this dongle's tuner is not one
                    // this driver supports yet"), and the Soapy failure that
                    // followed is a second symptom of the same radio.
                    return std::move(r);
                }
            } else {
                return std::move(r);  // r.dev stays null: the GUI thread reports it
            }
        }
        // A rate refusal is not fatal (the panel shows the actual readback
        // either way) but is surfaced - and so is a rate the driver coerced
        // on a call that succeeded.
        r.error = cascade::gui::applySourceRate(*dev, r.requestRateHz, r.error).sourceError;
        // AN RSP IS TUNED BEFORE ITS STREAM STARTS (0.99.36). The carry-across
        // tune below in finishDeviceOpen runs after setSource has started the
        // radio, so on an RSP it was a live sdrplay_api_Update milliseconds
        // after sdrplay_api_Init - and three field logs (0.95.0 and 0.96.2
        // RSP1A, 0.99.27 RSP1) show the service never answering exactly that
        // Update. Written here, while nothing streams, it is only the
        // parameter block and reaches the radio through Init; the later tune
        // then finds it already there and sends nothing (SdrPlaySource::
        // setCenterFrequencyHz). Why the service does not answer that early is
        // not known - there is no RSP on the bench - so this removes the call
        // rather than claiming to have explained it. The same order the
        // startup restore and the patch radios already use.
        // THROUGH THE CONVERTER (0.99.37): keepCenterHz is an AIR frequency;
        // launchDeviceOpen converted it for this radio as preTuneRadioHz.
        if (r.kind == "sdrplay" && r.preTuneRadioHz.has_value()) {
            (void) dev->setCenterFrequencyHz(*r.preTuneRadioHz);
        }
        r.dev = std::move(dev);
        return std::move(r);
    });
}

std::unique_ptr<cascade::source::DeviceSource> Engine::makeDeviceSource(
    const std::string& kind) {
    // ONE PLACE DECIDES WHAT A KIND NAME MEANS. The worker thread, the
    // synchronous config restore and the prefer-native fallback all construct
    // drivers, and three copies of this switch would be three chances for
    // "rtlsdr" to mean something different in one of them.
    if (testHooks_.makeDevice != nullptr) { return testHooks_.makeDevice(kind); }
    if (kind == "rtlsdr") { return std::make_unique<cascade::source::RtlSdrSource>(); }
    // Reached only from the patch page's radios: the Source section opens a
    // sound card through its own worker (app_window_soundcard.cpp).
    if (kind == "soundcard") { return std::make_unique<cascade::source::SoundCardSource>(); }
    if (kind == "hackrf") { return std::make_unique<cascade::source::HackRfSource>(); }
    if (kind == "airspy") { return std::make_unique<cascade::source::AirspySource>(); }
    if (kind == "airspyhf") { return std::make_unique<cascade::source::AirspyHfSource>(); }
    if (kind == "sdrplay") { return std::make_unique<cascade::source::SdrPlaySource>(); }
    if (kind == "mirisdr") { return std::make_unique<cascade::source::MiriSdrSource>(); }
    if (kind == "rx888") { return std::make_unique<cascade::source::Rx888Source>(); }
    if (kind == kPlutoDriverKey) { return std::make_unique<cascade::source::PlutoSource>(); }
    if (kind == "soapy") { return std::make_unique<cascade::source::SoapySource>(); }
    return nullptr;
}

void Engine::followSourceRowAfterRescan(const std::vector<cascade::gui::SourceRowKey>& rowsBefore) {
    // THE SELECTION, FOUND AGAIN BY WHAT IT IS (see the top of scanNative).
    // A native radio that is installed but was not the selected row (the combo
    // showed its live name, -1) is pointed at again when its row is back.
    sourceSel_ = cascade::gui::refindSourceRow(rowsBefore, sourceSel_, sourceRowKeys(), kNativeRowBase);
    if (sourceSel_ < 0 && device_ != nullptr && soapyView_ == nullptr) {
        for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
            if (nativeDevices_[i].driver == sourceKind_ && nativeDevices_[i].args == deviceArgs_) {
                sourceSel_ = kNativeRowBase + static_cast<int>(i);
            }
        }
    }
}

std::string Engine::nativeLabelFor(const std::string& kind, const std::string& args) const {
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        if (d.driver == kind && d.args == args) { return d.label; }
    }
    return args;
}

std::vector<cascade::gui::SourceRowKey> Engine::sourceRowKeys() const {
    std::vector<cascade::gui::SourceRowKey> keys;
    keys.reserve(static_cast<std::size_t>(soapyRowBase()) + soapyDevices_.size());
    keys.push_back({"siggen", ""});
    keys.push_back({"file", ""});
    // Row kSoundCardRow. Which card is not part of the row - the row is the
    // sound card section, whatever card it is set to.
    keys.push_back({"soundcard", ""});
    static_assert(kSoundCardRow == 2 && kNativeRowBase == kSoundCardRow + 1,
                  "sourceRowKeys pushes one key per fixed row, in combo order");
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // THE PLUTO ROW BY ITS FAMILY ALONE: there is only ever one, and its
        // args are whatever the address box held when the list was built, so
        // an address typed since would otherwise lose the selection.
        keys.push_back({d.driver, d.driver == kPlutoDriverKey ? std::string() : d.args});
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        keys.push_back({"soapy", d.args});
    }
    return keys;
}

void Engine::adoptDeviceMirrors(cascade::source::DeviceSource& dev, const std::string& kind,
                                   const std::string& args, double requestRateHz) {
    // ONE COPY OF THIS, shared by the worker-thread open and the synchronous
    // config restore. There were two before, and they had already drifted:
    // only one of them pointed the Rate combo at the device's ACTUAL readback
    // rather than at what was asked for.

    // THE RATE MENU IS THE DEVICE'S OWN LIST NOW. It was the fixed 1/2/4/8
    // MS/s table for every radio on every driver - which an RTL-SDR cannot
    // actually do two of (it has no 4 MS/s and no 8) and which stops 2.4 MS/s,
    // the rate ADS-B needs, from ever appearing.
    deviceRatesHz_ = dev.supportedSampleRatesHz();
    deviceRateLabels_.clear();
    for (const double r : deviceRatesHz_) { deviceRateLabels_.push_back(rateLabel(r)); }
    const double actualHz = dev.sampleRateHz();
    deviceRateIndex_ = nearestIndex(deviceRatesHz_, actualHz > 0.0 ? actualHz : requestRateHz);

    // AGC probe doubling as initialization: explicitly select manual gain
    // mode (matching the unchecked box). A device that says it has no gain
    // mode gets the documented "grey the checkbox" answer, not an error.
    deviceAgcSupported_ = dev.autoGainSupported() && dev.setAutoGain(false);
    deviceAgc_ = false;

    // THE GAINS, AND THEIR REAL RANGES. The sliders were 0..60 dB for every
    // stage of every radio; they are now what the driver says it will accept.
    deviceGainRanges_ = dev.gains();
    deviceGainNames_.clear();
    deviceGainsDb_.clear();
    for (const cascade::source::GainInfo& g : deviceGainRanges_) {
        deviceGainNames_.push_back(g.name);
        deviceGainsDb_.push_back(0.0f);
    }
    for (std::size_t i = 0; i < deviceGainNames_.size(); ++i) {
        if (kind == "soapy") {
            // UNCHANGED FOR A SOAPY DEVICE, deliberately. FoxSDR has always
            // pushed a mid-dial 30 dB into every stage of a Soapy radio at
            // open, because a vendor module's boot gain is whatever the last
            // application left behind, and the B200 on this bench has been
            // opened that way for months. Clamped into the stage's real range
            // now, which is the only difference: 30 dB into a MIXER that
            // stops at 16.1 used to be silently clamped by the driver and is
            // now clamped by something that can say what it did.
            const double want = std::min(std::max(static_cast<double>(kSoapyGainDefaultDb),
                                                  deviceGainRanges_[i].minDb),
                                         deviceGainRanges_[i].maxDb);
            dev.setGainDb(deviceGainNames_[i], want);
        }
        // A NATIVE DRIVER IS LEFT ALONE, and this is the difference that
        // matters. Both native drivers put the radio into a KNOWN STATE as
        // part of open() and say so in their headers - a middling manual gain
        // on the RTL-SDR, LNA 16 / VGA 16 / amp off on the HackRF - chosen for
        // that hardware. Overwriting it with a panel constant would throw away
        // the one thing the driver knows that the panel does not.
        deviceGainsDb_[i] = static_cast<float>(dev.gainDb(deviceGainNames_[i]));
    }

    // THE BIAS TEE, AFTER THE DEVICE IS UP, and only when the radio has one
    // this panel can reach. The six non-RTL drivers switch it OFF as part of
    // open(), so a remembered "on" is re-applied here or a mast-head
    // amplifier would go dark on every launch - but only THIS radio's own,
    // by driver and serial; an RTL-SDR gets back only what was remembered for
    // THAT dongle, and never an "on" on a dongle with no EEPROM. The checkbox
    // then shows the READBACK. A radio without one leaves every remembered
    // setting alone. The whole rule, and why, is biasTeeAfterOpen in
    // engine/bias_tee.hpp.
    biasTeeAfterOpen(biasTeePanel_, dev, args);

    // THE PER-RADIO SWITCHES, READ AND NOT WRITTEN. Unlike the bias tee these
    // carry no saved value to push (see app_window.hpp for why none of them
    // is persisted), so all this does is ask the driver what state its open()
    // left the radio in and mirror that. A readback rather than an assumption
    // for the same reason as everywhere else in this function: a panel that
    // showed a switch the hardware did not actually take is the lie the
    // antenna combo was fixed for.
    deviceRfNotchPresent_ = withRfNotch(&dev, [this](auto& d) {
        deviceRfNotch_ = d.rfNotch();
        return true;
    });
    deviceDabNotchPresent_ = withDabNotch(&dev, [this](auto& d) {
        deviceDabNotch_ = d.dabNotch();
        return true;
    });
    deviceHdrPresent_ = withHdrMode(&dev, [this](auto& d) {
        deviceHdr_ = d.hdrMode();
        return true;
    });
    deviceAdcSwitchesPresent_ = withAdcSwitches(&dev, [this](auto& d) {
        deviceDither_ = d.dither();
        deviceRandomiser_ = d.randomiser();
        return true;
    });

    // Antenna: apply a saved port if this device has one by that name, then
    // read back whatever is actually selected, so the panel never claims a
    // port the driver did not accept. Never guessed at - which port carries
    // an antenna is a fact about the user's cabling that no default can know.
    deviceAntennas_ = dev.antennas();
    if (!deviceAntenna_.empty()) { dev.setAntenna(deviceAntenna_); }
    deviceAntenna_ = dev.antenna();
}

std::unique_ptr<cascade::source::DeviceSource> Engine::openDeviceSync(
    const std::string& kind, const std::string& args, double requestRateHz) {
    // Also guards CONFIG RESTORE, not just the dropdown: a sound card saved by
    // an older build must not come back as the radio on every launch.
    if (kind == "soapy" && isAudioDriver(args)) {
        sourceError_ = "saved source was a sound card (driver=audio), not a radio - ignored";
        return nullptr;
    }
    std::unique_ptr<cascade::source::DeviceSource> dev = makeDeviceSource(kind);
    if (!dev) {
        sourceError_ = "saved source kind \"" + kind + "\" is not one this build can open";
        return nullptr;
    }
    if (!dev->open(args)) {
        sourceError_ = dev->lastError();
        return nullptr;
    }
    // A rate refusal is not fatal (the panel shows the actual readback
    // either way) but is surfaced - and so is a rate the driver coerced on a
    // call that succeeded.
    sourceError_ = cascade::gui::applySourceRate(*dev, requestRateHz, sourceError_).sourceError;
    adoptDeviceMirrors(*dev, kind, args, requestRateHz);
    return dev;
}

void Engine::setVfoToAbsoluteHz(double wantAbsHz, bool snap) {
    if (snap) {
        // Same raster as the drag path (kModeSnapHz); Shift bypasses it.
        const double s = kModeSnapHz[modeIndex_];
        wantAbsHz = std::round(wantAbsHz / s) * s;
    }
    double off = wantAbsHz - pipeline_.activeSource().centerFrequencyHz();
    // Keep the whole band inside the baseband +/- inputRate/2, exactly as the
    // drag path does — clicking near the panel edge must not park the filter
    // half outside the spectrum we actually receive. The same limit a sound
    // card's tune is judged against (gui::tuneWithFixedCentre), from one place.
    off = cascade::gui::vfoOffsetInsideSpan(off, pipeline_.inputRateHz(), vfoBandwidthHz_);
    pipeline_.setVfoOffsetHz(off);
    vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
}

const char* Engine::pluginQuarantineSuffix() { return ".disabled"; }

bool Engine::restoreQuarantinedPlugins(std::string& error) {
    error.clear();
    const std::string suffix = pluginQuarantineSuffix();
    std::error_code ec;
    const std::filesystem::path dir(pluginDir_);
    if (!std::filesystem::is_directory(dir, ec)) { return true; }

    // Collected first, renamed after: renaming inside a directory_iterator
    // walk is the classic way to get an implementation-defined half-listing.
    std::vector<std::string> quarantined;
    for (auto it = std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) { continue; }
        const std::string name = it->path().filename().string();
        if (name.size() <= suffix.size()) { continue; }
        if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) { continue; }
        // The live name this would restore to must be a name install() could
        // itself have written. Anything else is not ours and is left alone —
        // the same "unmanaged means unmanaged" rule the inventory follows.
        const std::string base = name.substr(0, name.size() - suffix.size());
        std::string safe;
        std::string sanErr;
        if (!cascade::core::PluginRepo::sanitiseFileName(base, safe, sanErr)) { continue; }
        quarantined.push_back(safe);
    }

    for (const std::string& file : quarantined) {
        const std::filesystem::path live = dir / file;
        const std::filesystem::path aside = dir / (file + suffix);
        std::error_code rec;
        if (std::filesystem::exists(live, rec)) {
            // An update (or a hand-install) landed while this copy was aside.
            // The live file is the one that counts; the leftover is stale
            // bytes under a hidden name, so it goes.
            std::filesystem::remove(aside, rec);
            if (rec) {
                error = "cannot delete the superseded \"" + aside.string() + "\": " + rec.message();
            }
            continue;
        }
        std::filesystem::rename(aside, live, rec);
        if (rec) {
            error = "cannot re-enable \"" + live.string() + "\": " + rec.message();
        }
    }
    return error.empty();
}

bool Engine::quarantineBlockedPlugins(std::string& error) {
    error.clear();
    const std::string suffix = pluginQuarantineSuffix();
    const std::filesystem::path dir(pluginDir_);
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) {
        std::string safe;
        std::string sanErr;
        if (!cascade::core::PluginRepo::sanitiseFileName(b.installed.file, safe, sanErr)) {
            // A record whose file name is not sanitisable names no file this
            // product could have installed, so there is nothing on disk to
            // move; the row is still shown as blocked.
            continue;
        }
        const std::filesystem::path live = dir / safe;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(live, ec)) { continue; }
        const std::filesystem::path aside = dir / (safe + suffix);
        std::filesystem::remove(aside, ec);  // a leftover from a crashed run
        std::filesystem::rename(live, aside, ec);
        if (ec) {
            error = "\"" + safe + "\" is out of date but could not be disabled (" +
                    ec.message() + "), so NO plugins were loaded this time.";
            return false;
        }
    }
    return true;
}

void Engine::detachAndUnloadPlugins() {
    // THE ORDER IS THE FEATURE, and it is why this is a function rather than
    // an open-coded sequence: it was open-coded, removeInstalledPlugin() then
    // called unloadAll() on its own, and the careful ordering below existed in
    // only one of the two places that unmap plugin modules.
    //
    // The RUNNER comes off first, in two steps. Detaching it from the pipeline
    // stops the DSP thread reaching it; clearing it then destroys the decoder
    // instances. Both must complete before unloadAll(), because a live handle
    // is memory inside a module about to be unmapped, and destroy() is code
    // inside that same module. Getting this order wrong is a crash in someone
    // else's DLL with no useful stack.
    pipeline_.setPluginRunner(nullptr);
    pluginRunner_.clear();
    // THE PATCH'S DECODERS, BY THE SAME RULE AND SYNCHRONOUSLY. A patch set
    // can hold instances of these very plugins (0.99.15), and unlike the
    // runner above it is not detached by a pointer - the DSP thread may be
    // inside it this instant. flushNow() shuts the DSP thread out, waits for
    // any block in progress, and destroys every set it holds before it
    // returns. The catalogue goes too: its API pointers point into the
    // modules about to go. The page rebuilds both from whatever loads next,
    // and the empty signature makes it republish.
    pipeline_.patchRunner().flushNow();
    // Every patch radio's runner too (0.99.17): each has its own reader
    // thread that may be inside a decoder this instant. The radios keep
    // running - only their sets go - and the cleared signatures rebuild them.
    for (auto& [node, radio] : patchRadios_) {
        (void)node;
        radio->runner().flushNow();
    }
    patchRadioSig_.clear();
    patchRefusedBy_.clear();
    patchCatalogue_.clear();
    patchApis_.clear();
    patchDspSig_.clear();
    patchRefused_.clear();
    // Same rule as the runner: a track-source or panel handle is memory inside
    // a module whose destroy() is code in that same module.
    pluginUi_.clear();
    // And the basemap, for exactly the same reason - its handle and its tile
    // borrows live in a module about to be unmapped.
    host_.onPluginsUnloading();

    pluginHost_.unloadAll();
}

std::vector<cascade::core::PluginUpdate> Engine::plannedPluginUpdates() const {
    // Pure, and empty until a catalogue has been fetched this session:
    // catalog_ is only ever filled by the plugin store window - its first
    // open in a session, or CHECK NOW.
    return cascade::core::PluginRepo::planUpdates(catalog_, pluginInventory_.plugins);
}

bool Engine::catalogEntryInstalled(const cascade::core::PluginCatalogEntry& e) const {
    const cascade::core::PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) { return false; }
    // Compare the SANITISED name — the exact name install() would write — so
    // a catalogue that spells its file oddly cannot make an already-installed
    // plugin look absent (and offer a second install that would then land
    // under a different name).
    std::string want;
    std::string err;
    if (!cascade::core::PluginRepo::sanitiseFileName(p->file, want, err)) { return false; }
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        const std::string have = std::filesystem::path(lp.path).filename().string();
        if (equalsFileNameAscii(have, want)) { return true; }
    }
    // The manifest too. A RETIRED plugin has been renamed out of the scan, so
    // the host has no record of it — but the file is very much installed, and
    // calling it absent would offer an Install where Update is the remedy.
    // missingFromDisk rows are excluded: those name a file the user deleted,
    // which really is not installed any more.
    for (const cascade::core::InstalledPlugin& ip : pluginInventory_.plugins) {
        if (!ip.missingFromDisk && equalsFileNameAscii(ip.file, want)) { return true; }
    }
    return false;
}

const cascade::core::LoadedPlugin* Engine::installedPluginRecord(
    const cascade::core::PluginCatalogEntry& e) const {
    const cascade::core::PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) { return nullptr; }
    // The SAME sanitised-name comparison catalogEntryInstalled makes, and for
    // the same reason: the name install() would write is the name to look for,
    // not the one the catalogue happens to spell.
    std::string want;
    std::string err;
    if (!cascade::core::PluginRepo::sanitiseFileName(p->file, want, err)) {
        return nullptr;
    }
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        const std::string have = std::filesystem::path(lp.path).filename().string();
        if (equalsFileNameAscii(have, want)) { return &lp; }
    }
    // No manifest fallback here, deliberately - see the header. A retired
    // module is installed and has no record; answering with something else's
    // record would be worse than answering "the host never saw it".
    return nullptr;
}

// THE REASONS ARE ENGLISH AND STAY ENGLISH HERE. ADD ALL compares one of them
// ("already installed"), the log records them and the web page is handed them,
// so each literal is marked FOX_TR_NOOP rather than translated, and every
// place that DRAWS one passes it through gui::trStoredReason(). The two that
// carry a value are made from the formats that function recognises.
std::string Engine::pluginInstallBlockedReason(int idx, bool acknowledged) const {
    if (idx < 0 || idx >= static_cast<int>(catalog_.size())) {
        return FOX_TR_NOOP("no plugin selected");
    }
    // One transfer at a time. PluginRepo has a single progress/cancel pair,
    // so two concurrent operations would share one progress bar and one
    // Cancel button — and a cancel would hit whichever happened to look.
    if (catalogPending_ || installPending_) {
        return FOX_TR_NOOP("a transfer is already in progress");
    }

    const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(idx)];
    // Same exact-match rule as the loader and as install() itself: a near-miss
    // ABI is how a struct layout change becomes memory corruption days later.
    if (e.abiVersion != static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
        return cascade::gui::pluginAbiMismatchReason(
            static_cast<unsigned>(e.abiVersion),
            static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
    }
    if (e.thisPlatform() == nullptr) {
        return cascade::gui::pluginNoBuildReason(std::string(cascade::core::PluginRepo::hostOs()) +
                                                 "/" + cascade::core::PluginRepo::hostArch());
    }
    // No licence, no install. The plugin host already refuses to LOAD a module
    // that declares no licence (PluginRejection::MissingLicence), so an entry
    // with no licence in the catalogue is at best a download that could never
    // run — and at worst code whose terms nobody can see. Either way the user
    // cannot have "seen the licence" if there is not one.
    if (e.licence.empty()) { return FOX_TR_NOOP("the catalogue entry declares no licence"); }
    if (catalogEntryInstalled(e)) { return FOX_TR_NOOP("already installed"); }
    // THE ACKNOWLEDGEMENT GATE, last so it is the final thing standing between
    // a compatible, licensed, not-yet-installed plugin and the download.
    if (!e.legalNotice.empty() && !acknowledged) {
        return FOX_TR_NOOP("the legal notice must be acknowledged first");
    }
    return {};
}

void Engine::startCatalogFetch() {
    if (catalogPending_ || installPending_) { return; }
    // A catalogue that could not be verified this time must not keep offering
    // installs from last time — the same all-or-nothing stance fetchIndex
    // takes with its own entries().
    catalog_.clear();
    // AND THE SELECTION AND THE CONSENT GO WITH IT. The store window clamps
    // its own selection every frame and clears its tick whenever the selection
    // moves, but the tick is consent for ONE plugin and a fetch that replaces
    // the whole catalogue can leave the same index pointing at a different
    // module - so it is cleared here, at the moment the ground moves, rather
    // than left to a rule that is about a different event. The ADD ALL tick
    // goes too: it was given against the notices the old catalogue listed.
    host_.onCatalogueFetchStarting();
    catalogError_.clear();
    catalogStatus_.clear();
    installError_.clear();
    installReport_.clear();
    catalogPending_ = true;

    const std::string url = pluginCatalogueUrl_;
    const std::string dir = pluginDir_;
    catalogFuture_ = std::async(std::launch::async, [this, url, dir] {
        CatalogFetchResult r;
        if (url.find("://") == std::string::npos) {
            // No scheme at all: a local file (see readLocalCatalogue). A URL
            // WITH a scheme — including http:// — goes to fetchIndex, which
            // is the single place the https-only rule is enforced.
            r.ok = readLocalCatalogue(url, r.entries, r.error);
        } else {
            r.ok = pluginRepo_.fetchIndex(url, r.error);
            if (r.ok) { r.entries = pluginRepo_.entries(); }
        }
        if (r.ok) {
            // THE ONLY MOMENT A RETIREMENT FLOOR IS WRITTEN TO THIS MACHINE.
            // Enforcement reads the cache and never the network, so a floor
            // that is not cached here protects nobody — not the user who goes
            // offline for a year, and not the one who never opens this browser
            // again. It runs on the worker because it re-hashes every
            // installed plugin.
            std::string policyError;
            if (!cascade::core::PluginRepo::cacheCataloguePolicies(dir, r.entries,
                                                                   policyError)) {
                r.policyError = "the catalogue loaded, but its plugin version policy could "
                                "not be saved, so it will not be remembered: " +
                                policyError;
            }
        }
        return r;
    });
}

void Engine::startInstall(cascade::core::PluginCatalogEntry entry) {
    if (catalogPending_ || installPending_) { return; }
    installError_.clear();
    installReport_.clear();
    installBusyName_ = entry.name;
    installPending_ = true;
    const std::string dir = pluginDir_;
    installFuture_ = std::async(
        std::launch::async, [this, e = std::move(entry), dir]() {
            PluginInstallResult r;
            r.name = e.name;
            // Every security rule lives inside install(): ABI match, platform
            // match, file-name sanitisation, https, the byte cap, and the
            // sha256 that decides whether the temp file ever becomes a plugin.
            r.ok = pluginRepo_.install(e, dir, r.installedPath, r.error);
            if (r.ok) {
                // MANIFEST UPKEEP, without which the whole retirement feature
                // silently no-ops: an unrecorded plugin has no id, no version
                // and no cached policy, so pluginBlockReason() fails open for
                // it forever. This is where a plain install becomes managed —
                // applyUpdate() records itself, so this is the only install
                // path that needs it.
                (void)cascade::core::PluginRepo::recordInstall(dir, e, r.recordError);
            }
            return r;
        });
}

void Engine::startUpdate(const cascade::core::PluginUpdate& u) {
    if (catalogPending_ || installPending_ || u.entry == nullptr) { return; }
    installError_.clear();
    installReport_.clear();
    installBusyName_ = u.entry->name;
    installPending_ = true;
    const std::string dir = pluginDir_;
    // The plan's entry aliases catalog_, which the GUI may replace while this
    // runs, so the worker owns a copy and the plan is re-pointed at it.
    installFuture_ = std::async(
        std::launch::async, [this, plan = u, entry = *u.entry, dir]() mutable {
            PluginInstallResult r;
            r.isUpdate = true;
            r.name = entry.name;
            plan.entry = &entry;
            // applyUpdate is install() plus recordInstall(), in that order and
            // with the same gauntlet — nothing here shortcuts it because "it
            // is only an update".
            r.ok = pluginRepo_.applyUpdate(plan, dir, r.installedPath, r.error);
            return r;
        });
}

void Engine::startAddAll(bool noticesAcknowledged) {
    if (addAllRun_.active || catalogPending_ || installPending_) { return; }
    // RE-PLANNED FROM LIVE STATE, not from the plan the key was drawn against.
    // The key was drawn one frame ago from a model built one frame ago, and
    // the whole point of this queue is that it acts over many frames.
    // THE PLAN IS THE STORE WINDOW'S (gui::planAddAll over the model it
    // builds, which reads its deck): asked for through the host, by catalogue
    // id - which is how the queue below has always named a module.
    const EngineHost::AddAllChoice plan = host_.planAddAll(noticesAcknowledged);
    if (!plan.blockedReason.empty()) {
        installError_ = plan.blockedReason;
        return;
    }

    addAllRun_ = AddAllRun{};
    for (const std::string& id : plan.installIds) {
        addAllRun_.ids.push_back(id);
        addAllRun_.isUpdate.push_back(false);
    }
    for (const std::string& id : plan.updateIds) {
        addAllRun_.ids.push_back(id);
        addAllRun_.isUpdate.push_back(true);
    }
    if (addAllRun_.ids.empty()) { return; }
    addAllRun_.active = true;
    addAllRun_.total = addAllRun_.ids.size();
    addAllSummary_.clear();
    addAllFailed_ = false;
    installError_.clear();
    installReport_.clear();
    cascade::core::diagLogf(
        "plugin store: add all starting - %zu to fetch, %zu to update, %zu passed over",
        plan.installIds.size(), plan.updateIds.size(), plan.skipped.size());
    for (const std::string& s : plan.skipped) {
        cascade::core::diagLogf("plugin store: add all - passing over %s", s.c_str());
    }
}

void Engine::pumpAddAll() {
    if (!addAllRun_.active) { return; }
    // A transfer is in flight, or a catalogue fetch is: wait. Nothing here
    // cancels anything - CANCEL is the user's key and it stops the transfer,
    // which this queue then records as a failure and carries on past.
    if (installPending_ || catalogPending_) { return; }

    while (addAllRun_.next < addAllRun_.ids.size()) {
        const std::string id = addAllRun_.ids[addAllRun_.next];
        const bool wantUpdate = addAllRun_.isUpdate[addAllRun_.next];
        ++addAllRun_.next;

        // THE ROW IS LOOKED UP AGAIN BY ID. catalog_ may have been replaced
        // since the queue was built, and an entry that is no longer there is a
        // failure with a reason rather than a silent skip.
        int idx = -1;
        for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
            if (catalog_[static_cast<std::size_t>(i)].id == id) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            ++addAllRun_.failed;
            addAllRun_.failures.emplace_back(id, FOX_TR_NOOP("no longer in the catalogue"));
            cascade::core::diagLogf(
                "plugin store: add all - %s FAILED: no longer in the catalogue", id.c_str());
            continue;
        }
        const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(idx)];
        addAllRun_.currentName = e.name;

        if (wantUpdate) {
            const std::vector<cascade::core::PluginUpdate> plans = plannedPluginUpdates();
            bool started = false;
            for (const cascade::core::PluginUpdate& u : plans) {
                if (u.id != id) { continue; }
                startUpdate(u);
                started = installPending_;
                break;
            }
            if (started) { return; }
            // The catalogue no longer offers a newer build for it - most
            // likely because the fetch that was already in flight installed
            // it. Not a failure; nothing is owed.
            continue;
        }
        // THE SAME GATE, RE-TESTED AT THE MOMENT IT STARTS. The queue was
        // planned frames ago and an install that has landed since can have
        // changed this answer - "already installed" most of all. Asked with
        // the notice acknowledged, because being IN this queue is the
        // acknowledgement the user gave at the key.
        const std::string blocked = pluginInstallBlockedReason(idx, true);
        if (!blocked.empty()) {
            if (blocked == "already installed") { continue; }
            ++addAllRun_.failed;
            addAllRun_.failures.emplace_back(e.name, blocked);
            cascade::core::diagLogf("plugin store: add all - %s FAILED: %s", e.name.c_str(),
                                    blocked.c_str());
            continue;
        }
        startInstall(e);
        if (installPending_) { return; }
    }

    // --- the queue is empty: say what happened ------------------------------
    //
    // TWICE, FROM THE SAME FACTS: in English for the log, and in the language
    // in force for the panel. The count is one format string, and each
    // reason is the English one translated where it is shown - PluginRepo's
    // own words, which no catalogue holds, come through unchanged.
    char buf[256];
    std::snprintf(buf, sizeof buf, "%d installed, %d failed", addAllRun_.installed,
                  addAllRun_.failed);
    std::string logged = buf;
    addAllSummary_ = cascade::core::formatText(tr("%d installed, %d failed"),
                              addAllRun_.installed, addAllRun_.failed);
    addAllFailed_ = addAllRun_.failed > 0;
    if (addAllRun_.failed > 0) {
        // NAMED, WITH THE REASON EACH GAVE. "3 failed" is a number nobody can
        // act on; the reason PluginRepo wrote is the only evidence the user
        // has, and it is carried through word for word.
        logged += ".";
        addAllSummary_ += ".";
        for (const auto& [name, reason] : addAllRun_.failures) {
            logged += " " + name + ": " + reason + ".";
            addAllSummary_ += " " + name + ": " + cascade::gui::trStoredReason(reason) + ".";
        }
    }
    cascade::core::diagLogf("plugin store: add all finished - %s", logged.c_str());
    addAllRun_.active = false;
    addAllRun_.currentName.clear();
    // THE TICK WAS FOR THIS RUN. It named the notices this run would take, and
    // the run has taken them; the next press is a new decision.
    host_.onAddAllFinished();
}

bool Engine::applyReceiverPosition(double latDeg, double lonDeg) {
    // REFUSED RATHER THAN CLAMPED, and by the same positive range test the
    // config sanitizer uses, so a typo cannot silently install a receiver at
    // the pole and quietly make every distance wrong. Every caller - the
    // toolbar's button, the satellites window's coordinate cells, a click on
    // that window's map - gets the same refusal, because a check written once
    // per entry point is a check that will eventually be missing from one.
    // The predicate also refuses 0,0 (receiverPositionAcceptable says why).
    if (!cascade::gui::receiverPositionAcceptable(latDeg, lonDeg)) { return false; }
    rxLat_ = latDeg;
    rxLon_ = lonDeg;
    rxSet_ = true;
    // The typed fields, every map page's home, the scope and the coverage map
    // follow - the window's half (AppWindow::onReceiverPositionApplied).
    host_.onReceiverPositionApplied(latDeg, lonDeg);
    return true;
}

void Engine::pollGpsReader() {
    cascade::core::NmeaFix fix;
    if (!gpsReader_.takeFix(fix)) { return; }
    // THE ONE DOOR. applyReceiverPosition moves every map page's home, tells
    // the scope, discards the coverage measured from the old origin and puts
    // the typed fields in step; a GPS fix that bypassed it would set a
    // position the rest of the application did not know about. It applies
    // the same predicate the reader already did, so a refusal here means the
    // two rules have drifted - said on screen and in the log (as a fact, not
    // a coordinate) rather than swallowed.
    if (applyReceiverPosition(fix.latDeg, fix.lonDeg)) {
        gpsRefusal_.clear();
        // The rail's "Receiver position" fold opens on this frame, because
        // rxSet_ just flipped and the row the user was watching (on the
        // rail's no-position block) is about to be replaced by that fold:
        // the Fixed status line has to be seen somewhere.
        host_.onGpsFixApplied();
        cascade::core::diagLogf("gps: fix applied as the receiver position");
    } else {
        gpsRefusal_ = tr("The GPS fix was refused by the position rule (off the globe, or 0,0).");
        cascade::core::diagWarnf("gps: fix refused by the acceptance rule");
    }
}

void Engine::rebuildPatchCatalogue() {
    patchCatalogue_.clear();
    patchApis_.clear();
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        // A plugin the user has STOPPED stays out of the patch too: a stop is
        // a decision about the plugin, not about one place it runs.
        const std::string key = cascade::core::pluginKey(p);
        if (pluginIsStopped(key)) { continue; }
        // One entry per decoder table, so a module with both an audio and an
        // I/Q decoder is two parts - with the same key and different inputs.
        if (p.decoder != nullptr) {
            cascade::core::patch::DecoderInfo info;
            info.key = key;
            info.name = p.name;
            info.feed = cascade::core::patch::PortType::Audio;
            info.requiredRateHz = static_cast<double>(p.decoder->requiredRateHz);
            info.tracks = p.trackSource != nullptr;   // it can put targets on a map
            patchCatalogue_.push_back(info);
            cascade::core::patch::PluginApis a;
            a.audio = p.decoder;
            patchApis_.push_back(a);
        }
        if (p.iqDecoder != nullptr) {
            cascade::core::patch::DecoderInfo info;
            info.key = key;
            info.name = p.name;
            info.feed = cascade::core::patch::PortType::Iq;
            info.requiredRateHz = p.iqDecoder->requiredRateHz;
            info.tracks = p.trackSource != nullptr;   // it can put targets on a map
            patchCatalogue_.push_back(info);
            cascade::core::patch::PluginApis a;
            a.iq = p.iqDecoder;
            patchApis_.push_back(a);
        }
        // A PICTURE decoder (APT, WEFAX, SSTV) - either input, through one
        // table - is a part too, showing its picture on its own face.
        if (p.imageDecoder != nullptr) {
            cascade::core::patch::DecoderInfo info;
            info.key = key + cascade::core::patch::kImageKeySuffix;
            info.name = p.name;
            info.feed = p.imageDecoder->inputKind == CASCADE_INPUT_IQ
                            ? cascade::core::patch::PortType::Iq
                            : cascade::core::patch::PortType::Audio;
            info.requiredRateHz = p.imageDecoder->requiredRateHz;
            info.image = true;
            info.tracks = p.trackSource != nullptr;   // it can put targets on a map
            patchCatalogue_.push_back(info);
            cascade::core::patch::PluginApis a;
            a.image = p.imageDecoder;
            patchApis_.push_back(a);
        }
    }
}

bool Engine::patchDecoderIsShown(cascade::core::patch::NodeId node) const {
    for (const cascade::core::patch::Wire& w : patchGraph_.wires()) {
        if (w.from != node) { continue; }
        const cascade::core::patch::Node* dst = patchGraph_.find(w.to);
        if (dst != nullptr && dst->kind == cascade::core::patch::NodeKind::Sink &&
            !dst->inputs.empty() && dst->inputs[0] == cascade::core::patch::PortType::Text) {
            return true;
        }
    }
    return false;
}

std::vector<cascade::gui::IndexedPreset> Engine::validatedPresets(
    const cascade::core::LoadedPlugin& p) const {
    std::vector<cascade::gui::IndexedPreset> out;
    if (p.preset == nullptr) { return out; }
    const std::uint32_t n =
        cascade::gui::cappedPresetCount(p.preset->count(), kMaxPresetsPerPlugin);
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        cascade::gui::IndexedPreset ip;
        ip.index = i;
        ip.preset.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
        if (p.preset->get(i, &ip.preset) != 1) { continue; }
        // Third-party numbers about to command a radio. A frequency that is
        // not a frequency is refused here rather than handed to a driver;
        // cascade::gui::presetIsValid is written as a positive test because
        // the negation would accept NaN.
        if (!cascade::gui::presetIsValid(ip.preset)) { continue; }
        out.push_back(ip);
    }
    return out;
}

std::vector<cascade::core::UserPreset> Engine::userPresetsForPlugin(
    const cascade::core::LoadedPlugin& p) const {
    return cascade::core::userPresetsFor(
        userPresets_, cascade::core::userPresetKey(cascade::core::pluginKey(p)));
}

void Engine::applyUserPresetEdit(const PendingUserPresetEdit& edit) {
    if (edit.op == PendingUserPresetEdit::Op::None) { return; }
    // Re-found by key, never carried from the press: the plugin may have been
    // removed between the press and this safe point, and a save against a
    // plugin that is no longer there is a silent no-op.
    const cascade::core::LoadedPlugin* found = nullptr;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (cascade::core::pluginKey(p) == edit.pluginFileKey) {
            found = &p;
            break;
        }
    }
    if (found == nullptr) { return; }
    const std::string key = cascade::core::userPresetKey(edit.pluginFileKey);

    if (edit.op == PendingUserPresetEdit::Op::Forget) {
        if (cascade::core::removeUserPreset(userPresets_, key, edit.ordinal)) {
            rebuildMuteStates();
            std::string forgot;
            cascade::core::formatUtf8(forgot, tr("Forgot a preset for %s"),
                          found->name.c_str());
            presetNote_ = forgot;
        }
        return;
    }

    // WHAT "WHERE YOU ARE" MEANS: the absolute tuned frequency (device centre
    // plus VFO offset - what the tuner readout shows), the mode button that
    // is lit, and the channel bandwidth. Not the device sample rate: that is
    // a property of the radio, not of the channel.
    cascade::core::UserPreset u;
    u.plugin = key;
    u.frequencyHz = pipeline_.activeSource().centerFrequencyHz() + pipeline_.vfoOffsetHz();
    u.demodMode = cascade::gui::abiDemodForModeIndex(modeIndex_);
    u.bandwidthHz = vfoBandwidthHz_;
    u.label = cascade::core::defaultUserPresetLabel(u.frequencyHz);
    std::string note;
    switch (cascade::core::addUserPreset(userPresets_, u)) {
        case cascade::core::UserPresetAdd::Added:
            rebuildMuteStates();
            cascade::core::formatUtf8(note, tr("Saved %.4f MHz as a preset for %s"),
                          u.frequencyHz / 1.0e6, found->name.c_str());
            break;
        case cascade::core::UserPresetAdd::AlreadySaved:
            cascade::core::formatUtf8(note, tr("%.4f MHz is already a preset for %s"),
                          u.frequencyHz / 1.0e6, found->name.c_str());
            break;
        case cascade::core::UserPresetAdd::PluginFull:
            cascade::core::formatUtf8(note,
                          tr("%s already has %u presets of yours - forget one to save another"),
                          found->name.c_str(),
                          static_cast<unsigned>(cascade::core::kMaxUserPresetsPerPlugin));
            break;
        case cascade::core::UserPresetAdd::ListFull:
            note = tr("No room for another preset");
            break;
        case cascade::core::UserPresetAdd::Invalid:
        default:
            note = tr("Not saved: no usable frequency to save");
            break;
    }
    presetNote_ = note;
}

void Engine::applyPluginTuneGrants() {
    // Every entry here is a PluginUi::tuneKey() - a module file name. A config
    // written by an older build holds DISPLAY NAMES instead; those match no
    // module, so such a grant reverts to its default of OFF and shows up as a
    // revocable row rather than quietly granting anything.
    for (const std::string& k : pluginTuneAllowed_) { pluginUi_.setTuneAllowed(k, true); }
}

void Engine::setPluginTuneAllowed(const std::string& pluginKey, bool allowed) {
    pluginUi_.setTuneAllowed(pluginKey, allowed);
    const auto it =
        std::find(pluginTuneAllowed_.begin(), pluginTuneAllowed_.end(), pluginKey);
    if (allowed && it == pluginTuneAllowed_.end()) {
        pluginTuneAllowed_.push_back(pluginKey);
    } else if (!allowed && it != pluginTuneAllowed_.end()) {
        pluginTuneAllowed_.erase(it);
    }
}

void Engine::applyPluginSettingsGrants() {
    for (const std::string& k : pluginSettingsAllowed_) { pluginUi_.setSettingsAllowed(k, true); }
}

void Engine::setPluginSettingsAllowed(const std::string& pluginKey, bool allowed) {
    // The live grant and the durable one in one place, exactly as the tune
    // grant does, so a grant that took effect can never fail to be saved.
    pluginUi_.setSettingsAllowed(pluginKey, allowed);
    const auto it =
        std::find(pluginSettingsAllowed_.begin(), pluginSettingsAllowed_.end(), pluginKey);
    if (allowed && it == pluginSettingsAllowed_.end()) {
        pluginSettingsAllowed_.push_back(pluginKey);
    } else if (!allowed && it != pluginSettingsAllowed_.end()) {
        pluginSettingsAllowed_.erase(it);
    }
}

void Engine::fillPublishedState(cascade::core::PublishedState& ps, const std::string& faultMessage,
                                   const cascade::core::RdsSnapshot& rds) {
    // Everything read here is read on the GUI thread, which is the contract
    // activeSource() and its readbacks require. Every figure a reader of the
    // receiver is told - a plugin, a browser, a logging program over CAT -
    // comes from these reads, once, so none of them can be told two
    // different things. Where a figure was read for the plugin API and for
    // the browser before stage 2, the expression is the one both used.
    // docs/engine-stage2.md section 2 maps every field to its source.
    FoxReceiverState& r = ps.rx;
    cascade::core::AppStateExt& e = ps.app;
    cascade::source::IqSource& src = pipeline_.activeSource();
    const cascade::sink::AudioOut& sink = pipeline_.audio();
    const bool running = pipeline_.running();

    // THE DECODERS, from ONE acquisition of the runner's lock (the lock its
    // DSP thread takes per block): how many instances are fed, and which
    // modules. The DEC lamp's predicate is running && active > 0; the DECODERS
    // card's count is the loaded decoder modules the runner feeds
    // (fedDecoderCount / loadedDecoderCount, asked the same way here).
    std::size_t runnerActive = 0;
    std::uint32_t fedModules = 0;
    std::uint32_t fittedModules = 0;
    if (running) { pluginRunner_.feedSnapshot(runnerActive, feedingKeysScratch_); }
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        if (p.decoder == nullptr && p.iqDecoder == nullptr && p.imageDecoder == nullptr) { continue; }
        ++fittedModules;
        if (!running) { continue; }
        const std::string key = cascade::core::pluginKey(p);
        if (key.empty()) { continue; }
        if (std::find(feedingKeysScratch_.begin(), feedingKeysScratch_.end(), key) !=
            feedingKeysScratch_.end()) {
            ++fedModules;
        }
    }

    // THE FLAGS: each condition, from its own source, into its own bit
    // (core::receiverFlags; test_snapshot_app walks them one at a time).
    cascade::core::RxFlagSources fs;
    fs.running = running;
    fs.deviceOpen = device_ != nullptr;
    fs.faulted = pipeline_.faulted();
    fs.muted = userMuted_;
    fs.stereoEnabled = stereoEnabled_;
    fs.stereoActive = pipeline_.stereoActive();
    fs.nr = nrEnabled_;
    fs.notch = notchEnabled_;
    fs.autoNotch = autoNotch_;
    fs.deviceAgc = deviceAgc_;
    fs.agcSupported = deviceAgcSupported_;
    fs.recordingIq = iqRecorder_.recording();
    fs.recordingAudio = audioRecorder_.recording();
    fs.scannerActive = scanner_.active();
    // The DEC lamp's own predicate (drawToolbar's MASTER cluster).
    fs.decoderActive = running && runnerActive > 0;
    fs.txAvailable = transmitter_.haveSink();
    fs.txKeyed = transmitter_.transmitting();
    fs.txLatched = transmitter_.latched();
    // The app's consent to a remote key: a transmitter open AND the Transmit
    // page on screen (what /api/status has always called transmitAvailable).
    fs.txRemoteArmed = transmitOpen_ && transmitter_.haveSink();
    fs.sinkOpen = sink.running();
    fs.webListening = host_.webListening();

    r.centreHz = src.centerFrequencyHz();
    r.vfoOffsetHz = pipeline_.vfoOffsetHz();
    r.tunedHz = r.centreHz + r.vfoOffsetHz;
    r.sampleRateHz = src.sampleRateHz();
    r.channelRateHz = pipeline_.channelRateHz();
    r.bandwidthHz = vfoBandwidthHz_;
    r.squelchDb = static_cast<double>(squelchDb_);
    r.volume = static_cast<double>(volume_);
    r.signalDb = static_cast<double>(pipeline_.signalPowerDb());
    // The host's own S-meter mapping (drawRadioSection): [-120, 0] dB.
    r.sMeter = std::clamp((r.signalDb + 120.0) / 120.0, 0.0, 1.0);
    // Not filled in stage 2: the VOLUME meter reads an audio tap the deck
    // drains itself (docs/engine-stage2.md, OPEN). -200 is "no level".
    r.audioLevelDb = -200.0;
    // The level-1 rule the plugin API has always used (the DSP's gate, with
    // its hysteresis, is not published by the pipeline).
    fs.squelchOpen = r.signalDb > r.squelchDb;
    r.flags = cascade::core::receiverFlags(fs);
    r.dbMin = static_cast<double>(dbMin_);
    r.dbMax = static_cast<double>(dbMax_);
    r.nrStrength = static_cast<double>(nrStrength_);
    r.notchHz = static_cast<double>(notchFreqHz_);
    r.notchQ = static_cast<double>(notchQ_);
    r.pilotLevel = static_cast<double>(pipeline_.pilotLevel());
    r.demodMode = cascade::gui::abiDemodForModeIndex(modeIndex_);
    r.deemphasis = static_cast<std::uint32_t>(deemphIndex_);
    r.gainCount = device_ != nullptr ? static_cast<std::uint32_t>(deviceGainRanges_.size()) : 0u;
    r.decodersRunning = fedModules;
    r.decodersFitted = fittedModules;
    r.txMode = static_cast<std::uint32_t>(transmitter_.mode());
    r.audioUnderruns = sink.underruns();
    r.txFrequencyHz = transmitter_.frequencyHz();
    r.txPowerDb = transmitter_.powerDb();
    r.txHoldRemainingMs = transmitter_.remoteHoldRemainingMs();
    r.txLatchRemainingMs = 0;  // OPEN: the Transmitter publishes no latch timer
    cascade::core::formatUtf8(r.deviceName, sizeof(r.deviceName), "%s",
                              src.name() != nullptr ? src.name() : "");
    // openedDeviceName() try-locks and answers "" while an open holds the
    // lock: that is not a change of sink, so the last good name is kept and
    // no counter moves.
    cascade::core::formatUtf8(
        r.sinkName, sizeof(r.sinkName), "%s",
        cascade::core::keepLastGoodName(sinkNameLastGood_, sink.openedDeviceName()).c_str());
    cascade::core::formatUtf8(r.faultMessage, sizeof(r.faultMessage), "%s", faultMessage.c_str());
    // r.txUnkeyReason: OPEN - the Transmitter keeps no sentence for it.

    // --- the host API level 1's tables (plugin get_gain / get_sample_rates) ---
    if (device_ != nullptr) {
        const std::size_t n =
            std::min<std::size_t>(deviceGainRanges_.size(), cascade::core::kMaxPublishedGains);
        for (std::size_t i = 0; i < n; ++i) {
            const cascade::source::GainInfo& g = deviceGainRanges_[i];
            cascade::core::PublishedGain& pg = e.gains[i];
            cascade::core::formatUtf8(pg.name, sizeof(pg.name), "%s", g.name.c_str());
            pg.unit = g.unit == cascade::source::GainUnit::Decibels ? CASCADE_GAIN_UNIT_DB
                                                                    : CASCADE_GAIN_UNIT_STEPS;
            pg.minDb = g.minDb;
            pg.maxDb = g.maxDb;
            pg.stepDb = g.stepDb;
            // THE READBACK MIRROR, as the sliders and the browser show it.
            pg.currentDb = i < deviceGainsDb_.size() ? static_cast<double>(deviceGainsDb_[i]) : 0.0;
        }
        e.abiGainCount = static_cast<std::uint32_t>(n);
        // The rates the Rate combo offers, from the same list it is built on.
        const std::size_t rn =
            std::min<std::size_t>(deviceRatesHz_.size(), cascade::core::kMaxPublishedRates);
        for (std::size_t i = 0; i < rn; ++i) { e.rates[i] = deviceRatesHz_[i]; }
        e.rateCount = static_cast<std::uint32_t>(rn);
    }
    e.outputRateHz = cascade::core::Pipeline::kAudioRateHz;
    e.outputFrames = pipeline_.audioSamplesProduced();

    // --- what /api/status carries that API 0.2 has no field for --------------
    e.rxPositionSet = rxSet_;
    e.rxLatDeg = rxLat_;
    e.rxLonDeg = rxLon_;
    e.autoNotchEngaged = pipeline_.autoNotchEngaged();
    e.autoNotchFreqHz = pipeline_.autoNotchFrequencyHz();
    e.pilotLocked = pipeline_.pilotLocked();
    e.rdsSynced = rds.synced;
    e.rdsPiValid = rds.state.piValid;
    e.rdsPi = rds.state.pi;
    e.rdsPsValid = rds.state.psValid;
    e.rdsPty = rds.state.pty;
    e.rdsTp = rds.state.tp;
    e.rdsTa = rds.state.ta;
    e.rdsGroups = rds.state.groupsDecoded;
    e.rdsErrors = rds.state.blockErrors;
    e.sourceBusy = soapyScanPending_ || deviceOpenPending_;
    {
        const double rateHz = cascade::core::Pipeline::kAudioRateHz;
        e.audioPrimingCallbacks = sink.primingCallbacks();
        e.audioRingMs = 1000.0 * static_cast<double>(sink.ringFrames()) / rateHz;
        e.audioRingCapacityMs = 1000.0 * static_cast<double>(sink.ringCapacityFrames()) / rateHz;
    }
    e.audioPluginGaps = pluginRunner_.audioGaps();
    e.audioPluginGapFrames = pluginRunner_.audioGapFrames();
    e.iqBytes = iqRecorder_.bytesWritten();
    e.audioBytes = audioRecorder_.bytesWritten();
    switch (scanner_.state()) {
        case cascade::core::Scanner::State::Idle: e.scannerState = cascade::core::kScannerIdle; break;
        case cascade::core::Scanner::State::Scanning: e.scannerState = cascade::core::kScannerScanning; break;
        case cascade::core::Scanner::State::Paused: e.scannerState = cascade::core::kScannerPaused; break;
        case cascade::core::Scanner::State::Holding: e.scannerState = cascade::core::kScannerHolding; break;
    }
    e.scanStartHz = scanStartMhz_ * 1.0e6;
    e.scanStopHz = scanStopMhz_ * 1.0e6;
    e.scanStepHz = scanStepKhz_ * 1.0e3;
    e.catalogueBusy = catalogPending_ || installPending_;
    const EngineHost::BasemapFacts basemap = host_.basemapFacts();
    e.basemapActive = basemap.active;
    e.basemapMinZoom = basemap.minZoom;
    e.basemapMaxZoom = basemap.maxZoom;
    e.basemapTileSize = basemap.tileSize;
}

std::size_t Engine::loadedDecoderCount() const {
    // THE RUNNER'S OWN TEST, and deliberately not a capability-bit test of its
    // own: PluginRunner::rebuild creates an instance when a module supplies a
    // decoder, an iqDecoder or an imageDecoder TABLE, and the host only fills
    // those pointers when the module declared the matching capability AND
    // supplied the table behind it (a declared capability with no table is a
    // refused load, PluginRejection::MissingDecoderApi). Asking the same
    // question the same way is what keeps this denominator and the runner's
    // activeCount numerator counting the same population.
    std::size_t n = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        if (p.decoder != nullptr || p.iqDecoder != nullptr || p.imageDecoder != nullptr) {
            ++n;
        }
    }
    return n;
}

std::size_t Engine::fedDecoderCount() const {
    std::size_t n = 0;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        if (p.decoder == nullptr && p.iqDecoder == nullptr && p.imageDecoder == nullptr) {
            continue;
        }
        if (pluginRunner_.isFeeding(cascade::core::pluginKey(p))) { ++n; }
    }
    return n;
}

bool Engine::pluginIsStopped(const std::string& pluginKey) const {
    // An empty key is what a record with no path produces; it must never
    // match, or one stray entry would stop every path-less plugin at once.
    if (pluginKey.empty()) { return false; }
    return std::find(pluginsStopped_.begin(), pluginsStopped_.end(), pluginKey) !=
           pluginsStopped_.end();
}

void Engine::recordPluginStopped(const std::string& pluginKey, bool stopped) {
    if (pluginKey.empty()) { return; }
    const auto it = std::find(pluginsStopped_.begin(), pluginsStopped_.end(), pluginKey);
    if (stopped && it == pluginsStopped_.end()) {
        pluginsStopped_.push_back(pluginKey);
    } else if (!stopped && it != pluginsStopped_.end()) {
        pluginsStopped_.erase(it);
    }
    // Down into both halves immediately. refreshPluginRunner pushes them again
    // before every rebuild, but a caller that only records (the preset path
    // does) still leaves the live objects agreeing with the durable list.
    pluginRunner_.setStopped(pluginsStopped_);
    pluginUi_.setStopped(pluginsStopped_);
    // The mute snapshot holds the same running state and is read every frame,
    // so it has to follow here too, not only at the next rebuild.
    rebuildMuteStates();
}

std::string Engine::pluginKeyForDisplayName(const std::string& displayName) const {
    if (displayName.empty()) { return {}; }
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.name == displayName) { return cascade::core::pluginKey(p); }
    }
    return {};
}

bool Engine::pluginMutes(const cascade::core::LoadedPlugin& p) const {
    const bool def = cascade::core::muteDefaultForCaps(p.capabilities);
    const std::string key = cascade::core::pluginKey(p);
    if (key.empty()) { return def; }
    const bool overridden = std::find(pluginMuteOverride_.begin(),
                                      pluginMuteOverride_.end(),
                                      key) != pluginMuteOverride_.end();
    return overridden ? !def : def;
}

void Engine::setPluginMutes(const cascade::core::LoadedPlugin& p, bool mutes) {
    const std::string key = cascade::core::pluginKey(p);
    if (key.empty()) { return; }
    // Stored as a DIFFERENCE from the capability default (see the AppConfig
    // note): choosing the default removes the entry, so a config never carries
    // a redundant override that a later improvement to the default rule could
    // not reach.
    const bool wantOverride = (mutes != cascade::core::muteDefaultForCaps(p.capabilities));
    const auto it = std::find(pluginMuteOverride_.begin(), pluginMuteOverride_.end(), key);
    if (wantOverride && it == pluginMuteOverride_.end()) {
        pluginMuteOverride_.push_back(key);
    } else if (!wantOverride && it != pluginMuteOverride_.end()) {
        pluginMuteOverride_.erase(it);
    }
    rebuildMuteStates();
}

void Engine::rebuildMuteStates() {
    muteStates_.clear();
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        cascade::core::MutePlugin m;
        m.key = cascade::core::pluginKey(p);
        m.name = p.name;
        // RUNNING MEANS ACTUALLY DECODING, and both halves are needed.
        //
        // isFeeding alone would be right but LATE: the stop list is pushed
        // down before the runner is rebuilt (see recordPluginStopped, which
        // records without rebuilding at all on the preset path), so a plugin
        // stopped a moment ago still has its instances and would keep the
        // audio muted until the next rebuild - the exact opposite of what
        // stopping it was for.
        //
        // pluginIsStopped alone is what the audio mute used to ask, and it
        // cannot tell an idle plugin from a working one: with the generator at
        // 2 MS/s and the receiver on 162.000 MHz the Sinks panel read "Muted by
        // AIS" while the Plugins panel read that AIS needs 192 kHz raw I/Q and
        // was not being fed. Silence on behalf of a decoder the application
        // itself says is doing nothing is silence for a reason that is not a
        // reason.
        m.running = !pluginIsStopped(m.key) && pluginRunner_.isFeeding(m.key);
        m.mutes = pluginMutes(p);
        // THE SAME ENUMERATION drawPluginPresets and the preset bars use - one
        // walk of this plugin's table, capped and filtered once, rather than
        // this function repeating a second copy of the cap and the frequency
        // sanity test. This is also THE reason a bar never calls count()/get()
        // itself: this cache is rebuilt on every plugin-set change (see the
        // callers of rebuildMuteStates), and m.presets - with the RAW index
        // and the bounded label a bar needs to draw and to record a press by
        // - is everything cascade::gui::presetBarKeys needs afterwards.
        for (const cascade::gui::IndexedPreset& ip : validatedPresets(p)) {
            const CascadePreset& ps = ip.preset;
            cascade::core::MutePreset mp;
            mp.frequencyHz = ps.frequencyHz;
            mp.bandwidthHz = (ps.bandwidthHz > 0.0 && ps.bandwidthHz < 1e12)
                                 ? ps.bandwidthHz
                                 : 0.0;
            mp.deviceCentre = (ps.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u;
            mp.index = ip.index;
            mp.label = cascade::gui::presetLabel(ps.label, p.name);
            m.presets.push_back(mp);
        }
        // AND THE USER'S OWN (0.99.4), after the plugin's, recorded in their
        // own index range so a bar's key press says which table to re-read.
        // Being in this list is also what lets the audio mute treat a saved
        // channel exactly as it treats one of the plugin's.
        const std::vector<cascade::core::UserPreset> mine = userPresetsForPlugin(p);
        for (std::size_t i = 0; i < mine.size(); ++i) {
            m.presets.push_back(cascade::gui::userPresetToMutePreset(mine[i], i));
        }
        muteStates_.push_back(std::move(m));
    }
}

std::string Engine::muteNameList(const std::vector<std::string>& names) {
    // "A and B" for two, "A, B and C" beyond: engine/tune_control.hpp, where the
    // mute banner's test measures the same sentence the banner draws.
    return cascade::gui::joinMuteNames(names);
}

std::string Engine::muteSubjectText() const {
    return muteNameList(mutedBy_);
}

void Engine::updateAudioMute() {
    cascade::core::TunePoint tune;
    tune.deviceCentreHz = pipeline_.activeSource().centerFrequencyHz();
    tune.tunedHz = tune.deviceCentreHz + pipeline_.vfoOffsetHz();

    const cascade::core::MuteDecision d =
        cascade::core::muteActive(muteStates_, tune);

    // ARE THE PLUGINS THAT WERE MUTING STILL RUNNING? This is what separates
    // "the user tuned away" from "the user stopped the plugin" - both end the
    // mute, and only the first is worth a dialog. Asked about the specific
    // plugins named in mutedByKeys_ rather than about muting plugins in
    // general; see the note on cascade::core::anyStillRunning for the case
    // that proved the difference on the real application.
    const bool stillRunning =
        cascade::core::anyStillRunning(muteStates_, mutedByKeys_);

    // THE EDGE, evaluated before anything is changed.
    const bool edge = cascade::core::tuneAwayEdge(mutePrevOnPreset_, d.active);
    if (edge && stillRunning) {
        muteKeptRunning_ = true;  // the audio stays down until they answer
    }
    // WHAT THE DIALOG IS ABOUT is decided here, once, from the names that were
    // muting on the frame the user left the preset - and is withdrawn here too
    // when arriving on a preset makes the question moot, or when the plugins it
    // named stop. mutedBy_/mutedByKeys_ still hold the latched names at this
    // point (they are only recomputed below, and only while ON a preset), so
    // the edge captures the plugins the user actually tuned away from.
    const bool wasOpen = mutePopup_.open;
    mutePopup_ = cascade::core::advanceMutePopup(mutePopup_, d.active, edge,
                                                 stillRunning, mutedBy_, mutedByKeys_);
    if (mutePopup_.open && !wasOpen) {
        mutePopupQueued_ = true;
    } else if (!mutePopup_.open) {
        // A withdrawal has to cancel a queue as well as an open window: the
        // queue is one frame long, and opening a dialog the frame after the
        // radio arrived on a preset would ask the question in the one place it
        // must never be asked.
        mutePopupQueued_ = false;
    }
    mutePrevOnPreset_ = d.active;

    // Coming BACK to a preset clears the latch, which is what re-arms the
    // popup: the user has to leave again to be asked again. So does the last
    // of those plugins stopping - there is then nothing to keep quiet for, and
    // a banner offering to stop a plugin that is not running would be a button
    // that does nothing.
    if (d.active || !stillRunning) { muteKeptRunning_ = false; }

    const bool muted = d.active || muteKeptRunning_;
    if (d.active) {
        mutedBy_ = d.names;
        mutedByKeys_ = d.keys;
    } else if (!muted) {
        mutedBy_.clear();
        mutedByKeys_.clear();
    }
    // ...and when the latch is holding, mutedBy_ deliberately KEEPS the names
    // from the frame the user tuned away on. They are the plugins the banner
    // is about, and recomputing them off-preset would empty the list and leave
    // a banner that could not say what was muting anything.

    // THE USER'S OWN MUTE IS OR'd IN HERE and nowhere else. It is deliberately
    // not folded into `muted` above: everything above is recomputed from the
    // tuning every frame, so a user mute stored in the same place would be
    // wiped the moment a decoder's preset decided anything, and the Mute key
    // would appear to stop working. The two mutes are separate facts about the
    // same audio; the pipeline is told their OR.
    // AND THE THIRD FACT ABOUT THIS AUDIO: whether the radio is transmitting.
    // A receiver left unmuted on the frequency it is transmitting on is a
    // howl, and on a full-duplex board like the Pluto the receiver really is
    // still running while the key is down - which is the point of keeping it
    // running, but not the point of listening to it. The operator can turn
    // the monitor on (transmitMonitor_), which is what somebody working split
    // wants; off is the default and is what everybody else wants.
    //
    // Separate from the other two for the same reason they are separate from
    // each other: it is recomputed from the KEY every frame, and folding it
    // into either of the others would wipe whichever one it was folded into.
    const bool txMute = transmitter_.transmitting() && !transmitMonitor_;
    pipeline_.setAudioMuted(muted || userMuted_ || txMute);
}

void Engine::pumpDecoderOutput() {
    // Called from drawUi every frame, NOT from the panel. The runner's queue
    // is bounded and drops silently by design (the DSP thread must never
    // block on the GUI), so draining only while the Plugins section happened
    // to be expanded would quietly lose decodes the user never knew existed.
    for (cascade::core::DecodedLine& l : pluginRunner_.drainText()) {
        // COUNTED HERE BECAUSE THIS IS THE ONLY PLACE THAT SEES THEM ALL.
        // decoderLog_ is a bounded tail and drops its oldest entries, so its
        // size is not a count of anything; the status column's rate card
        // differences this cumulative figure over a window instead.
        ++decoderLinesTotal_;
        decoderLog_.push_back(std::move(l));
    }
    // The track-info plugin's status ("cannot reach the registry") lands in
    // the same log: it is the only place a user looks when a plugin is quiet.
    for (std::string& s : host_.drainTrackInfoText()) {
        cascade::core::DecodedLine l;
        l.plugin = "Aircraft info";
        l.text = std::move(s);
        decoderLog_.push_back(std::move(l));
    }
    // THE PATCH'S DECODERS, drained every frame for the same reason the
    // runner's are: its queue is bounded and drops. A line is SHOWN only when
    // its node's Text output is wired to a Text sink - that wire is what the
    // Text out part is for, and a decoder wired to nothing is marked on the
    // canvas as having nobody listening. Tagged with the NODE's name, so two
    // POCSAG decoders on two frequencies read as two sources, not one.
    // Pictures: the newest per node, kept for its face.
    // From the receiver's patch runner and from EVERY patch radio's own
    // (0.99.17) - a decoder runs on whichever radio it is wired to.
    std::vector<cascade::core::patch::Runner*> patchRunners{&pipeline_.patchRunner()};
    for (auto& [rid, radio] : patchRadios_) {
        (void)rid;
        patchRunners.push_back(&radio->runner());
    }
    std::vector<cascade::core::patch::PatchLine> patchLines;
    for (cascade::core::patch::Runner* pr : patchRunners) {
        for (auto& [node, img] : pr->drainImages()) { host_.onPatchPicture(node, std::move(img)); }
        for (cascade::core::patch::PatchLine& pl : pr->drainText()) {
            patchLines.push_back(std::move(pl));
        }
    }
    for (cascade::core::patch::PatchLine& pl : patchLines) {
        {
            PatchDecoderFace& face = patchDecoderFaces_[pl.node];
            face.last = pl.text;
            ++face.lines;
        }
        if (patchFirstLineLogged_.insert(pl.node).second) {
            // THAT a decoder produced text, and how much - never the text.
            // It is decoded traffic (a pager message, an aircraft), which
            // PRIVACY.md says no report carries, and this line used to carry
            // the first 160 characters of it.
            cascade::core::diagLogf("patch: first line from decoder node %u%s (%zu characters)",
                                    static_cast<unsigned>(pl.node),
                                    patchDecoderIsShown(pl.node) ? "" : " (not wired to a Text out)",
                                    pl.text.size());
        }
        if (!patchDecoderIsShown(pl.node)) { continue; }
        // Onto the face of every Text out this decoder is wired to.
        for (const cascade::core::patch::Wire& w : patchGraph_.wires()) {
            if (w.from != pl.node) { continue; }
            const cascade::core::patch::Node* dst = patchGraph_.find(w.to);
            if (dst == nullptr || dst->kind != cascade::core::patch::NodeKind::Sink) { continue; }
            std::deque<std::string>& q = patchSinkLines_[w.to];
            q.push_back(pl.source + ": " + pl.text);
            while (q.size() > 200) { q.pop_front(); }
        }
        ++decoderLinesTotal_;
        cascade::core::DecodedLine l;
        l.plugin = std::move(pl.source);
        l.text = std::move(pl.text);
        decoderLog_.push_back(std::move(l));
    }
    while (decoderLog_.size() > kDecoderLogMax) { decoderLog_.pop_front(); }
}

void Engine::openTransmitRadio() {
    transmitError_.clear();
    // SEEDED FROM THE RECEIVER when the receiver is a Pluto, because one board
    // doing both is the overwhelmingly common case and making the operator
    // retype an address they have already given is how a transmitter ends up
    // pointed at the wrong machine on a shared bench.
    if (transmitArgs_.empty()) {
        transmitArgs_ = (sourceKind_ == "pluto" && !deviceArgs_.empty())
                            ? deviceArgs_
                            : std::string("uri=ip:192.168.2.1");
    }
    auto tx = std::make_unique<cascade::source::PlutoTx>();
    if (!tx->open(transmitArgs_)) {
        transmitError_ = tx->lastError();
        cascade::core::diagWarnf("tx: could not open %s - %s", transmitArgs_.c_str(),
                                 transmitError_.c_str());
        return;
    }
    // THE POWER IS WRITTEN BEFORE THE SINK IS INSTALLED, and it is written
    // through the driver's own clamp - so a config file carrying a number
    // from a board with a different range lands on that board's quiet end
    // rather than on its loud one (source/pluto_tx.hpp, clampTxGainDb).
    tx->setGainDb(transmitPowerDb_);
    transmitPowerDb_ = tx->gainDb();
    transmitter_.setSink(std::move(tx));
    transmitter_.setMode(cascade::dsp::txModeFromIndex(transmitModeIndex_));
    transmitter_.setInput(cascade::core::txInputFromIndex(transmitInputIndex_));
    transmitter_.setToneHz(transmitToneHz_);
    followTransmitFrequency();
}

void Engine::closeTransmitRadio() {
    // setSink(nullptr) stops and destroys whatever is there, which silences
    // it: the driver's stop() and its destructor both do, and Transmitter
    // unkeys before either runs.
    transmitter_.setSink(nullptr);
    transmitError_.clear();
}

void Engine::followTransmitFrequency() {
    if (!transmitter_.haveSink()) { return; }
    const double want = cascade::gui::txFrequencyHz(transmitSplit_, transmitSplitHz_,
                                                    std::max(0.0, currentAbsoluteHz()));
    // ONLY WHEN IT HAS MOVED. This runs every frame and a Pluto's retune is a
    // WRITE on a socket; writing the same number a thousand times a second
    // would saturate the control connection and leave nothing for the
    // controls a hand is actually operating.
    if (std::fabs(want - transmitter_.frequencyHz()) < 1.0) { return; }
    if (!transmitter_.setFrequencyHz(want)) { transmitError_ = transmitter_.lastError(); }
}

void Engine::loadBandPlan() {
    const std::string dir = cascade::core::BandPlan::defaultDir();
    std::error_code ec;
    // Existence is checked FIRST so the overwhelmingly common "no band plans
    // installed" case stays completely silent, per the feature's contract;
    // only a directory that is really there can produce an error worth
    // showing.
    if (!std::filesystem::is_directory(std::filesystem::path(dir), ec)) { return; }
    bandPlanChoices_ = cascade::core::BandPlan::available(dir);
    // Cleared before every attempt so that picking a working plan after a
    // failed one actually clears the red text — a stale error would outlive
    // the condition that caused it and make a healthy overlay look broken.
    bandPlanError_.clear();
    std::string err;
    if (!bandPlan_.loadSelection(dir, bandPlanSelection_, err)) { bandPlanError_ = err; }
}

void Engine::stopIqRecording() {
    // Order per the Pipeline::setIqRecorder contract: after the setter
    // returns no writeIq against this recorder is in flight or can begin
    // (the pointer swap serializes on the mutex the DSP thread holds across
    // writes), so stop() — which patches the header and closes the file —
    // cannot overlap a write. Both calls are no-ops when already idle.
    pipeline_.setIqRecorder(nullptr);
    iqRecorder_.stop();
}

void Engine::stopAudioRecording() {
    pipeline_.setAudioRecorder(nullptr);
    audioRecorder_.stop();
}

bool Engine::endTakes(bool iq, bool audio, const char* why, bool asError) {
    const bool endIq = iq && iqRecorder_.recording();
    const bool endAudio = audio && audioRecorder_.recording();
    if (!endIq && !endAudio) { return false; }
    if (endIq) { stopIqRecording(); }
    if (endAudio) { stopAudioRecording(); }
    // Said, not left to be noticed: the Recorder section and the web page
    // both show it, and the next Record press clears it.
    const char* what = (endIq && endAudio) ? "I/Q and audio recordings" :
                       endIq               ? "I/Q recording"
                                           : "audio recording";
    // Plain English, like the source errors beside it: a tr() key here
    // would need an entry in every catalogue under resources/lang.
    const std::string line = std::string("The ") + what + " ended because " + why +
                             ((endIq && endAudio) ? ". The files are closed and complete"
                                                  : ". The file is closed and complete") +
                             "; press Record to start a new one.";
    // An ERROR (a fault) is kept beside whatever error was already showing -
    // a refused start, say - rather than written over it: the earlier one is
    // still true and may be the more useful of the two. A NOTICE (a source
    // the user chose to change) is not an error at all and must not light the
    // web page's FAIL lamp, which reads recordError; it has its own field.
    std::string& slot = asError ? recordError_ : recordNotice_;
    if (slot.empty()) {
        slot = line;
    } else if (slot.find(line) == std::string::npos) {
        slot += " " + line;
    }
    if (asError) {
        cascade::core::diagWarnf("recorder: %s ended because %s", what, why);
    } else {
        cascade::core::diagLogf("recorder: %s ended because %s", what, why);
    }
    return true;
}

void Engine::stopReceiver() {
    // Play-stop while recording stops the recording cleanly (spec): taps
    // uninstalled and both WAVs finalized BEFORE the DSP threads join, so a
    // take can never outlive the sample flow it was taping. Only when the
    // receiver is running or faulted - see the header for why a stop that
    // stops nothing leaves an armed take alone. Silent, as it always was: the
    // user pressed Stop, and "the recording ended because you stopped" is
    // not news.
    if (pipeline_.running() || pipeline_.faulted()) {
        stopIqRecording();
        stopAudioRecording();
    }
    // Unconditional, as applyControlRequest's own stop always was (the dome,
    // the key and POWER only ever call this on a running receiver): on a
    // faulted pipeline, run flag already down, it still joins the threads
    // the fault left behind. Joins within ~10 ms, an acceptable one-off hitch
    // on the GUI thread for a Stop.
    pipeline_.stop();
}

void Engine::endTakesOnFault() {
    // On the EDGE, not the level: a take the user arms after seeing FAIL is
    // their choice (it waits for Play like any take armed while stopped), and
    // ending it every frame would make Record look broken while the lamp is
    // lit. Pipeline::start clears the latch, so the next fault is a new edge.
    const bool faulted = pipeline_.faulted();
    if (faulted && !faultSeen_) { endTakes(true, true, "the receiver stopped on a fault", true); }
    faultSeen_ = faulted;
}

void Engine::installSource(std::unique_ptr<cascade::source::IqSource> src) {
    // BEFORE the swap: setSource keeps the DSP thread running across it, so
    // a take ended afterwards would already hold the first blocks of the new
    // source.
    endTakes(true, false, "the source changed", false);
    pipeline_.setSource(std::move(src));
}

bool Engine::startAudioRecording() {
    if (audioRecorder_.recording()) { return true; }
    std::string err;
    if (!audioRecorder_.start(cascade::core::RecordKind::Audio, recordDir_,
                              cascade::core::Pipeline::kAudioRateHz, err)) {
        recordError_ = err;
        return false;
    }
    recordError_.clear();
    recordNotice_.clear();
    audioRecordStartS_ = host_.frameTimeS();
    // Install AFTER start(): the tap must never feed a recorder that is not
    // accepting (Pipeline::setAudioRecorder contract).
    pipeline_.setAudioRecorder(&audioRecorder_);
    return true;
}

bool Engine::startIqRecording() {
    // The Record IQ key's body and the web remote's recordIq, which were two
    // copies of the same six lines until stage 1. Baseband at the DSP input
    // rate through the pipeline's raw tap.
    if (iqRecorder_.recording()) { return true; }
    const double rate = pipeline_.inputRateHz();
    std::string err;
    if (!iqRecorder_.start(cascade::core::RecordKind::BasebandIq, recordDir_, rate, err)) {
        recordError_ = err;
        return false;
    }
    recordError_.clear();
    recordNotice_.clear();
    iqRecordRateHz_ = rate;
    iqRecordStartS_ = host_.frameTimeS();
    // Install AFTER start(): the tap must never feed a recorder that is not
    // accepting (Pipeline::setIqRecorder contract).
    pipeline_.setIqRecorder(&iqRecorder_);
    return true;
}

void Engine::addBookmarkHere(const std::string& name) {
    cascade::core::Bookmark b;
    b.name = name;
    if (b.name.empty()) {
        // A nameless row would render blank; default to the frequency.
        char def[32];
        std::snprintf(def, sizeof(def), "%.4f MHz", currentAbsoluteHz() / 1.0e6);
        b.name = def;
    }
    // "Current" is the tuned station: center readback + VFO offset (the band
    // the spectrum overlay marks), with the live mode and the REQUESTED
    // bandwidth (the overlay's value, pre any Vfo clamp).
    b.freqHz = currentAbsoluteHz();
    b.mode = kModeNames[modeIndex_];
    b.bandwidthHz = vfoBandwidthHz_;
    freqMgr_.add(std::move(b));
    saveBookmarks();
}

cascade::core::Scanner::Params Engine::scannerParams() const {
    // The stored parameters -> Params. Scanner::configure sanitizes (swap,
    // step floor, negative times), so the raw values can be handed over.
    cascade::core::Scanner::Params p;
    p.startHz = scanStartMhz_ * 1.0e6;
    p.stopHz = scanStopMhz_ * 1.0e6;
    p.stepHz = scanStepKhz_ * 1.0e3;
    p.dwellMs = scanDwellMs_;
    p.holdMs = scanHoldMs_;
    p.resumeMs = scanResumeMs_;
    p.listenMs = scanListenMs_;
    return p;
}

std::vector<cascade::gui::RunnableDecoder> Engine::runnableDecoders() const {
    // Built from the runner rather than from the loaded list: "running" here
    // means BEING FED, which is the only sense in which a decoder is costing
    // this machine anything (running_view.hpp says why).
    std::vector<cascade::gui::RunnableDecoder> runnable;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (!lp.loaded) { continue; }
        const bool isDec = lp.decoder != nullptr || lp.iqDecoder != nullptr ||
                           lp.imageDecoder != nullptr;
        if (!isDec) { continue; }
        cascade::gui::RunnableDecoder rd;
        rd.name = lp.name;
        rd.key = cascade::core::pluginKey(lp);
        rd.stopped = pluginIsStopped(rd.key);
        rd.feeding = pluginRunner_.isFeeding(rd.key);
        runnable.push_back(std::move(rd));
    }
    return runnable;
}

void Engine::setModeIndex(int index) {
    if (index < 0 || index >= 8) { return; }
    modeIndex_ = index;
    pipeline_.setDemodMode(kModeMap[index]);
    bandwidthIndex_ = kModeDefaultBw[index];
    vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    // The MODE and its bandwidth - a demodulator change, not a tuning change.
    // No frequency reaches the log, here or anywhere else.
    cascade::core::diagLogf("mode: %s, bandwidth %.0f", kModeNames[index], vfoBandwidthHz_);
}

void Engine::importBookmarkFile(const std::string& path) {
    std::string p = path;
    // A path pasted from Explorer's "Copy as path" arrives quoted.
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') { p = p.substr(1, p.size() - 2); }
    const auto t0 = std::chrono::steady_clock::now();
    cascade::core::ImportResult r = cascade::core::importFrequencyFile(p);
    if (!r.error.empty() && r.items.empty()) {
        bookmarkImportNote_ = cascade::core::formatText(tr("Could not import: %s"), r.error.c_str());
        // The file's KIND, never its name or path: "never the name or path of
        // a file you opened" (PRIVACY.md), and a frequency list's name is
        // usually what is on it.
        // importFrequencyFile's "cannot open" names the path; the log does not.
        std::string why = r.error;
        for (std::size_t at = why.find(p); !p.empty() && at != std::string::npos;
             at = why.find(p, at)) {
            why.replace(at, p.size(), "(the file)");
        }
        cascade::core::diagWarnf("bookmarks: an import (%s) failed: %s",
                                 std::filesystem::path(p).extension().string().c_str(),
                                 why.c_str());
        return;
    }
    const std::size_t found = r.items.size();
    const std::size_t added = freqMgr_.addMany(std::move(r.items));
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char note[320];
    cascade::core::formatUtf8(note, sizeof(note),
                  "%s: %zu entries read, %zu added%s%s%s", r.format.c_str(), found, added,
                  found > added ? " (the rest were already here)" : "",
                  r.skipped > 0 ? ", some had no usable frequency" : "",
                  r.shifted > 0 ? ", converter Shift values were not applied" : "");
    bookmarkImportNote_ = note;
    cascade::core::diagLogf("bookmarks: imported a %s file - %zu read, %zu added, %zu skipped, %zu shifted, %.0f ms",
                            r.format.c_str(), found, added, r.skipped, r.shifted, ms);
    if (added > 0) { saveBookmarks(); }
}

void Engine::saveBookmarks() {
    if (bookmarkPath_.empty()) { return; }  // hermetic run: never touch disk
    bookmarkSaveDirty_ = true;
    bookmarkSaveDueS_ = host_.frameTimeS() + 1.0;
}

void Engine::flushBookmarkSave(bool force) {
    if (!bookmarkSaveDirty_ || bookmarkPath_.empty()) { return; }
    if (!force && host_.frameClockRunning() && host_.frameTimeS() < bookmarkSaveDueS_) { return; }
    bookmarkSaveDirty_ = false;
    std::string err;
    if (freqMgr_.save(bookmarkPath_, err)) {
        bookmarkError_.clear();  // a successful save clears a stale error
    } else {
        bookmarkError_ = err;
    }
}

void Engine::noteTuneRefused(double requestHz, bool isPluginPreset) {
    // THE OTHER HALF OF "SAY WHEN THE RADIO CANNOT TUNE THERE": a radio that
    // refuses outright leaves the counter exactly where it was, and until this
    // existed nothing anywhere said why. Shown where the coerced-tune sentence
    // is shown and logged once per distinct request, for the same reason
    // noteTuneMismatch logs once. cascade::gui::tuneRefusedMessage answers
    // nothing for a refusal INSIDE the radio's range, which is a busy driver
    // and not a fact about the radio.
    double rangeLoHz = 0.0;
    double rangeHiHz = 0.0;
    const bool hasRange = device_ != nullptr && device_->frequencyRangeHz(rangeLoHz, rangeHiHz);
    // requestHz is an AIR frequency (activeSource() speaks air). With a
    // converter on, the sentence is the converter's, in air terms; without
    // one, air and radio are the same number and the plain sentence speaks.
    const cascade::core::ConverterSetting conv = pipeline_.converter();
    const std::string note =
        cascade::core::converterActive(conv)
            ? converterTuneNote(requestHz, /*refused=*/true, 0.0, isPluginPreset)
            : cascade::gui::tuneRefusedMessage(requestHz, hasRange, rangeLoHz, rangeHiHz,
                                               isPluginPreset);
    if (note.empty()) { return; }
    tuneMismatchNote_ = note;
    if (requestHz == lastRefusedRequestHz_) { return; }
    lastRefusedRequestHz_ = requestHz;
    // WHERE the request fell against the range, never either frequency: what
    // somebody tunes to must not reach a report (PRIVACY.md), and "below its
    // range" diagnoses the refusal as well as the number did. Judged at the
    // RADIO, which is where the range is.
    const double radioHz = cascade::core::radioFromAir(conv, requestHz);
    const char* where = !cascade::core::airReachable(conv, requestHz)
                            ? "(beyond what the converter can deliver)"
                        : !hasRange             ? "(it publishes no range)"
                        : radioHz < rangeLoHz   ? "below its range"
                        : radioHz > rangeHiHz   ? "above its range"
                                                : "inside its range";
    cascade::core::diagLogf("source: the %s refused a tune %s", pipeline_.activeSource().name(),
                            where);
}

void Engine::noteTuneMismatch(double requestHz, double answeredHz, bool isPluginPreset) {
    double rangeLoHz = 0.0;
    double rangeHiHz = 0.0;
    // device_ is null for the generator and the IQ file — neither has a range
    // to ask about, and tuneMismatchMessage already knows what an absent one
    // means (drop the range sentence rather than print a sentinel as if it
    // were a fact).
    const bool hasRange = device_ != nullptr && device_->frequencyRangeHz(rangeLoHz, rangeHiHz);
    // Both figures are AIR frequencies; with a converter on the sentence says
    // so in the converter's terms (see noteTuneRefused).
    const cascade::core::ConverterSetting conv = pipeline_.converter();
    tuneMismatchNote_ =
        cascade::core::converterActive(conv)
            ? converterTuneNote(requestHz, /*refused=*/false, answeredHz, isPluginPreset)
            : cascade::gui::tuneMismatchMessage(requestHz, answeredHz, hasRange, rangeLoHz,
                                                rangeHiHz, isPluginPreset);
    if (tuneMismatchNote_.empty()) { return; }
    // ONCE PER DISTINCT REQUEST. A repeated identical command (a user pressing
    // the same preset twice, or the scanner dwelling on a frequency the radio
    // refuses) lands on the same wrong answer every time; logging it again on
    // every occurrence would fill the diagnostics bundle with one repeated
    // line and push everything else out of it.
    if (requestHz == lastMismatchRequestHz_ && answeredHz == lastMismatchAnswerHz_) {
        return;
    }
    lastMismatchRequestHz_ = requestHz;
    lastMismatchAnswerHz_ = answeredHz;
    // HOW FAR OFF and whether the radio clamped to its range, never either
    // frequency (see noteTuneRefused). The error in ppm alone does not say
    // what was tuned.
    // At the RADIO: that is where the range and the synthesiser are.
    const double requestRadioHz = cascade::core::radioFromAir(conv, requestHz);
    const double answeredRadioHz = cascade::core::radioFromAir(conv, answeredHz);
    const bool atEdge = hasRange && (std::fabs(answeredRadioHz - rangeLoHz) < 1.0 ||
                                     std::fabs(answeredRadioHz - rangeHiHz) < 1.0);
    const double ppm = (requestRadioHz != 0.0)
                           ? (answeredRadioHz - requestRadioHz) / requestRadioHz * 1.0e6
                           : 0.0;
    cascade::core::diagLogf("source: the %s answered a tune somewhere else (%s, %+.0f ppm)",
                            pipeline_.activeSource().name(),
                            atEdge ? "at the edge of its range" : "not at a range edge", ppm);
}

double Engine::currentAbsoluteHz() {
    return pipeline_.activeSource().centerFrequencyHz() + pipeline_.vfoOffsetHz();
}

void Engine::fillStatusLists(cascade::net::RadioStatus& s, const std::string& faultMessage,
                                const cascade::core::RdsSnapshot& rds,
                                std::vector<std::uint64_t>& bookmarkIds) {
    // THE TEXT AND THE LISTS of /api/status, as the browser has always been
    // sent them: every std::string and std::vector member of RadioStatus, and
    // nothing else - the figures come from the PublishedState
    // (net::composeRadioStatus puts the two together on the reader's
    // thread). Everything read here is read on the GUI thread, which is the
    // contract activeSource() and its readbacks require.
    cascade::source::IqSource& src = pipeline_.activeSource();
    s.faultMessage = faultMessage;
    s.sourceName = src.name();  // copied into a std::string here, deliberately
    // The frequency readout's face, as the NAME the page and the config file
    // both speak - see RadioStatus::tunerDisplayStyle.
    s.tunerDisplayStyle = host_.tunerDisplayStyle();
    s.sourceKind = sourceKind_;
    // THE TRANSMITTER, AS THE BROWSER SEES IT (0.95.1): transmitting,
    // transmitAvailable and the hold are figures (fillPublishedState:
    // TX_KEYED, TX_REMOTE_ARMED, txHoldRemainingMs).
    //
    // transmitAvailable IS TWO CONDITIONS, and the second one is the point: a
    // radio has to be open AND the transmit page has to be on screen. Opening
    // a transmitter is already a deliberate act at this machine, and requiring
    // the page as well means a remote key can only ever be closed while the
    // operator has the transmitter in front of them - so the state the page
    // shows is the state somebody is looking at. Closing the page revokes the
    // remote key exactly as it releases the local PTT.
    s.soapyArgs = deviceArgs_;
    s.antenna = deviceAntenna_;
    s.antennas = deviceAntennas_;
    s.sourceError = sourceError_;
    // NATIVE ROWS FIRST, exactly as the desktop combo orders them, so the
    // browser's list and the application's list are the same list.
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        s.devices.push_back({d.label, d.args, d.driver});
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        s.devices.push_back({d.label, d.args, "soapy"});
    }
    // THE DEVICE THAT IS ALREADY OPEN MUST APPEAR IN THE LIST even when no
    // scan has run this session. soapyDevices_ is filled only by an explicit
    // scan (enumeration walks the USB bus, so it is never done automatically),
    // but a device restored from the config is open and receiving — and a
    // source list that omitted it showed only "Signal generator" while the
    // radio was plainly working, with no way to select it back after switching
    // away.
    if (device_ != nullptr && !deviceArgs_.empty()) {
        bool listed = false;
        for (const cascade::net::RadioStatus::SoapyDevice& d : s.devices) {
            if (d.args == deviceArgs_ && d.kind == sourceKind_) {
                listed = true;
                break;
            }
        }
        if (!listed) {
            s.devices.insert(s.devices.begin(), {s.sourceName, deviceArgs_, sourceKind_});
        }
    }
    for (std::size_t i = 0; i < deviceGainNames_.size(); ++i) {
        const double db = (i < deviceGainsDb_.size())
                              ? static_cast<double>(deviceGainsDb_[i])
                              : 0.0;
        // WITH THE UNIT, so the browser can letter the number the same way
        // the desktop does. The value field keeps its name and its meaning -
        // a page written against an older build still reads it - and "unit"
        // says whether it is decibels or the radio's own steps.
        s.gains.push_back({deviceGainNames_[i], db, cascade::gui::gainUnitWire(gainUnitAt(i))});
    }

    // The same names the Sinks panel shows, from the same source, so the two
    // clients cannot disagree about why the radio is quiet.
    s.audioMutedBy = muteSubjectText();
    // WHO THE SPEAKERS BELONG TO, from the same runner the SINK card asks, so
    // the browser and the bench cannot tell different stories about what is
    // coming out of this radio. Empty is the ordinary case: the demodulated
    // audio is playing and the two gap counters (figures) have nothing to
    // report.
    s.audioSource = pluginRunner_.playingPlugin();
    s.recordDir = recordDir_;
    s.recordError = recordError_;
    s.recordNotice = recordNotice_;

    // A FEW HUNDRED AT MOST go to the browser: an imported list of 33 000
    // would be copied every frame and serialised on every poll, for a page
    // that can only ever show a screenful. Favourites first, then the ones
    // nearest the tuned frequency; `bookmarkIds` maps the browser's row
    // numbers to the bookmarks' IDS for tune and remove - ids, not list
    // indices, because a desktop add, remove or star applied before the next
    // web request (the command drain runs first in a frame) shifts every index
    // after it, and a stale index names a different bookmark. The ids travel
    // IN THE PUBLISHED BLOCK with these rows (applyControlRequest reads them
    // from readFull()), so the map and the rows /api/status serves are always
    // from the same publish.
    {
        const std::vector<cascade::core::Bookmark>& all = freqMgr_.list();
        bookmarkIds.clear();
        for (const std::size_t i : freqMgr_.nearestSubset(currentAbsoluteHz(), 300, 100)) {
            const cascade::core::Bookmark& b = all[i];
            bookmarkIds.push_back(b.id);
            s.bookmarks.push_back({b.name, b.freqHz, b.mode, b.bandwidthHz});
        }
    }

    for (const cascade::core::HostTrack& t : pluginUi_.tracks()) {
        // THE SAME STALENESS RULE THE DESKTOP MAP APPLIES, so the two views do
        // not disagree about what is still flying. A dropped target simply
        // stops appearing in the snapshot; ageMs is still published for the
        // ones that remain, so the web page is free to fade them on its own
        // terms - that presentation is the web UI's decision, not this one's.
        if (!cascade::core::trackPresentation(t.t.ageMs, t.t.kind).visible) { continue; }
        cascade::net::RadioStatus::Track w;
        w.id = t.t.id;
        w.label = t.t.label;
        w.plugin = t.plugin;
        w.latDeg = t.t.latDeg;
        w.lonDeg = t.t.lonDeg;
        w.altM = t.t.altM;
        w.courseDeg = t.t.courseDeg;
        w.speedMps = t.t.speedMps;
        w.ageMs = t.t.ageMs;
        w.kind = t.t.kind;
        w.flags = t.t.flags;
        // Enrichment from the track-info plugin. Running every frame for
        // every live target, this loop is ALSO what drives the lookups: the
        // ask is non-blocking, PENDING costs nothing, and answers are cached.
        host_.enrichWebTrack(w);
        s.tracks.push_back(std::move(w));
    }

    host_.fillWebImages(s);

    s.basemap.attribution = host_.basemapFacts().attribution;

    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        cascade::net::RadioStatus::Plugin w;
        w.name = p.name;
        w.version = p.version;
        w.licence = p.licence;
        w.loaded = p.loaded;
        w.error = p.error;
        // Also the tune-grant key, and taken from the one place that computes
        // it so the two can never drift: the browser echoes this string back
        // to toggle the permission.
        w.fileName = cascade::core::PluginUi::tuneKey(p);
        // Keyed on the same file name, and read from the same durable list the
        // desktop row reads, so the browser cannot claim a stopped plugin is
        // running.
        w.stopped = pluginIsStopped(w.fileName);
        w.canRequestTune = (p.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u;
        // Keyed on the module file (w.fileName), never on the display name:
        // the browser sends that same key back to toggle the grant, and a name
        // is the plugin's own to choose.
        w.tuneAllowed = std::find(pluginTuneAllowed_.begin(), pluginTuneAllowed_.end(),
                                  cascade::core::PluginUi::tuneKey(p)) !=
                        pluginTuneAllowed_.end();
        // Declared presets, from the SAME enumeration drawPluginPresets and
        // the preset bars use: capped, because a plugin is third-party code
        // and a list this long is not a menu, and each frequency positively
        // tested so a value that is not a frequency (NaN included) never
        // reaches the browser or, through it, a driver.
        for (const cascade::gui::IndexedPreset& ip : validatedPresets(p)) {
            const CascadePreset& ps = ip.preset;
            cascade::net::RadioStatus::Plugin::Preset wp;
            wp.label = cascade::gui::presetLabel(ps.label, p.name);
            wp.frequencyHz = ps.frequencyHz;
            wp.bandwidthHz = ps.bandwidthHz;
            wp.sampleRateHz = ps.sampleRateHz;
            w.presets.push_back(std::move(wp));
        }
        s.plugins.push_back(std::move(w));
    }

    s.catalogueStatus = catalogStatus_;
    s.catalogueError = catalogError_;
    s.installReport = installReport_;
    s.installError = installError_;
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(i)];
        cascade::net::RadioStatus::CatalogEntry c;
        c.id = e.id;
        c.name = e.name;
        c.version = e.version;
        c.licence = e.licence;
        c.summary = e.summary;
        c.legalNotice = e.legalNotice;
        c.installed = catalogEntryInstalled(e);
        // THE SAME predicate the desktop's Install button consults, asked as
        // "would this be installable if the notice were acknowledged" — so the
        // browser and the window can never disagree about what may be
        // installed, and the notice itself is still a separate, explicit act.
        c.blockedReason = pluginInstallBlockedReason(i, /*acknowledged=*/true);
        s.catalogue.push_back(std::move(c));
    }

    // The tail of the decoder log. Bounded here rather than sending the whole
    // deque: this is a live readout, and the panel the desktop shows is a tail
    // too.
    {
        constexpr std::size_t kMaxWebDecoded = 60;
        const std::size_t total = decoderLog_.size();
        const std::size_t from = (total > kMaxWebDecoded) ? total - kMaxWebDecoded : 0;
        for (std::size_t i = from; i < total; ++i) {
            s.decoded.push_back({decoderLog_[i].plugin, decoderLog_[i].text});
        }
    }
    // RDS text; the RDS figures are in the PublishedState.
    s.rdsPs = rds.state.ps;
    s.rdsRadioText = rds.state.radioText;
}

void Engine::telemetryAccrueMode() {
    // Seconds are banked against the mode that was ACTUALLY running, once per
    // frame. Accumulating against the mode selected at shutdown would credit
    // the whole session to whatever happened to be last.
    //
    // The accrual carries the sub-second remainder between calls. It has to:
    // a frame is ~17 ms, so every individual delta truncates to zero seconds
    // and nothing would ever be banked.
    const std::uint64_t secs = telemetryModeAccrual_.advance(host_.wallTimeS());
    if (secs > 0) { telemetryModeSeconds_[kModeNames[modeIndex_]] += secs; }
}

void Engine::telemetryNotePanel(const char* name) {
    if (name == nullptr || name[0] == '\0') { return; }
    const std::string n(name);
    if (telemetryPanels_.size() >= 16) { return; }
    if (std::find(telemetryPanels_.begin(), telemetryPanels_.end(), n) ==
        telemetryPanels_.end()) {
        telemetryPanels_.push_back(n);
    }
}

void Engine::telemetryStartup(const cascade::core::AppConfig& cfg) {
    telemetryEnabled_ = cfg.telemetryEnabled;
    telemetryInstallId_ = cfg.telemetryInstallId;
    // Reporting is on by default, so a first run arrives here enabled with no
    // identifier. Mint one now. If the CSPRNG fails there is no id, and
    // reporting stays off rather than falling back to anything guessable.
    if (telemetryEnabled_ && telemetryInstallId_.empty()) {
        telemetryInstallId_ = cascade::core::newInstallId();
        telemetryEnabled_ = !telemetryInstallId_.empty();
    }
    telemetryLaunches_ = cfg.telemetryLaunches + 1;
    telemetryCrashes_ = cfg.telemetryCrashes;
    // The previous run never wrote its clean-exit marker, so it did not end
    // normally. This is the whole crash-counting mechanism: no crash handler,
    // no minidump, nothing uploaded from the failure itself - just the
    // observation that last time the marker was never set.
    if (!cfg.telemetryCleanExit) { ++telemetryCrashes_; }
    // (The same marker is also the window's trigger to offer a crash report,
    // and the crash-loop limiter's memory is read beside it: both are the
    // window's diagnostics, AppWindow::diagnosticsStartup.)
    telemetrySessionStart_ = host_.wallTimeS();
    telemetryModeAccrual_.reset(telemetrySessionStart_);
    // Last session's report goes now, on a thread, while the window is coming
    // up. Nothing waits for it and nothing reports if it fails.
    if (telemetryEnabled_ && !telemetryInstallId_.empty() &&
        !cfg.telemetryPending.empty()) {
        telemetryReporter_.send(cascade::core::telemetryEndpoint(), cfg.telemetryPending);
    }
    // Heartbeats are NOT armed here: this runs for bounded --frames runs too,
    // and arming them for those put ctest's throwaway install ids on the live
    // endpoint. run() arms them, interactive runs only, beside the update
    // check that follows the same rule.
}

void Engine::telemetryJournal(cascade::core::AppConfig& cfg) {
    // (The update and diagnostics settings that used to ride along here are
    // the window's: AppWindow::diagnosticsJournal writes them just before.)
    cfg.telemetryEnabled = telemetryEnabled_;
    cfg.telemetryInstallId = telemetryInstallId_;
    cfg.telemetryLaunches = telemetryLaunches_;
    cfg.telemetryCrashes = telemetryCrashes_;
    cfg.telemetryPending.clear();
    if (!telemetryEnabled_ || telemetryInstallId_.empty()) {
        return;  // opted out: nothing is written, so nothing can later be sent
    }
    telemetryAccrueMode();

    cascade::core::TelemetryReport r;
    r.installId = telemetryInstallId_;
    r.appVersion = cascade::versionString();
    r.os = cascade::core::osDescription();
    r.arch = cascade::core::archDescription();
    r.launches = telemetryLaunches_;
    r.crashes = telemetryCrashes_;
    const double now = host_.wallTimeS();
    r.session.seconds = static_cast<std::uint64_t>(
        now > telemetrySessionStart_ ? now - telemetrySessionStart_ : 0.0);
    r.session.modeSeconds = telemetryModeSeconds_;
    r.session.panels = telemetryPanels_;
    // MODEL ONLY - sanitiseDevice strips the serial, which the raw args carry
    // twice (once alone, once inside the label).
    r.session.sdrModel = deviceModel_;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && r.session.plugins.size() < 20) {
            r.session.plugins.push_back(p.name + " " + p.version);
        }
    }
    cfg.telemetryPending = r.toJson();
}

}  // namespace cascade::engine
