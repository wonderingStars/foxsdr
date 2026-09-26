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

// THE RSP's THREE OTHER SWITCHES, EACH ONE DRIVER WIDE. Same argument as
// withBiasTee - they are not ports and not gains, and DeviceSource would have
// to invent an answer for every source that has no such thing - but each of
// these is offered by exactly one driver, so there is no dispatch to do
// beyond the cast, and the per-MODEL question is the one that matters: an
// RSP1A has no HDR mode, an RSPdx has no DAB notch, and the driver answers
// for the device that is actually open rather than for the family.
template <typename Fn>
bool withRfNotch(cascade::source::DeviceSource* dev, Fn&& fn) {
    auto* sp = dynamic_cast<cascade::source::SdrPlaySource*>(dev);
    if (sp == nullptr || !sp->rfNotchSupported()) { return false; }
    return fn(*sp);
}

template <typename Fn>
bool withDabNotch(cascade::source::DeviceSource* dev, Fn&& fn) {
    auto* sp = dynamic_cast<cascade::source::SdrPlaySource*>(dev);
    if (sp == nullptr || !sp->dabNotchSupported()) { return false; }
    return fn(*sp);
}

template <typename Fn>
bool withHdrMode(cascade::source::DeviceSource* dev, Fn&& fn) {
    auto* sp = dynamic_cast<cascade::source::SdrPlaySource*>(dev);
    if (sp == nullptr || !sp->hdrModeSupported()) { return false; }
    return fn(*sp);
}

// THE RX888's ADC PAIR, and they travel together because they are one GPIO
// word and one decision: dither trades a little noise floor for spurs that
// stop sitting on exact frequencies, and the output randomiser undoes the
// FX3's own scrambling. The randomiser is ONE switch on purpose - the driver
// flips the chip and the host-side de-randomiser in the same call, because
// turning it on at the chip alone turns the whole band into noise.
template <typename Fn>
bool withAdcSwitches(cascade::source::DeviceSource* dev, Fn&& fn) {
    auto* r = dynamic_cast<cascade::source::Rx888Source*>(dev);
    if (r == nullptr) { return false; }
    return fn(*r);
}

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

// The Display sliders keep at least this many dB between min and max: a
// thinner span renders as a near-solid waterfall and a wall-to-wall trace,
// and a zero/inverted span would degrade to the widgets' flat-line fallback.
constexpr float kMinDbSpan = 10.0f;

constexpr double kSoapyRateHz[] = {1.0e6, 2.0e6, 4.0e6, 8.0e6};

constexpr int kSoapyRateDefaultIndex = 1;  // 2 MS/s

constexpr float kSoapyGainDefaultDb = 30.0f;

// Per-mode default bandwidth (index into kBwHz), applied when a mode button
// is clicked; the combo still allows any override. Rationale: WFM broadcast
// channel 150k; NFM two-way channel 12.5k; AM/DSB broadcast channel ~10k
// (both sidebands); SSB/CW voice/keying fits in 3k; RAW passes the full
// 200k channel for diagnostics.
constexpr int kModeDefaultBw[8] = {2, 1, 3, 3, 5, 5, 5, 0};

// SoapyAudio advertises every sound card on the machine as a SoapySDR device.
// They are not receivers: no tuner (centerFrequencyHz reads 0), no RF, and
// selecting one silently swaps your radio for a microphone input — which then
// gets persisted to config and restored on the next launch, so the real SDR
// appears to have "stopped being detected". They are filtered out of the
// Source list entirely, matching the policy --soapy-check already applies.
bool isAudioDriver(const std::string& args) {
    return args.find("driver=audio") != std::string::npos;
}

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

// Index of the value in arr[0..n) closest to x (ties resolve low). Used to
// point preset combos at whatever a config file or device readback holds.
int nearestIndex(const double* arr, int n, double x) {
    int best = 0;
    for (int i = 1; i < n; ++i) {
        if (std::fabs(arr[i] - x) < std::fabs(arr[best] - x)) { best = i; }
    }
    return best;
}

// The same for a device's own rate list, which is what the Rate combo shows
// now - twelve rows on an RTL-SDR, ten on a HackRF, four on a Soapy driver
// that reports none. Empty answers 0, which is the "no selection" the combo
// draws as blank rather than reading past the end of a vector.
int nearestIndex(const std::vector<double>& v, double x) {
    if (v.empty()) { return 0; }
    return nearestIndex(v.data(), static_cast<int>(v.size()), x);
}

// Scanner user-tune detection slack, Hz. Far above double rounding through
// (absHz - offset) + offset (nano-Hz at 9.99 GHz) and far below the smallest
// manual tuning action (the readout's 1 Hz digit), so it can neither
// false-trigger on arithmetic noise nor miss a real user tune.
constexpr double kScanUserTuneEpsHz = 0.5;

// What the Sinks panel says while a device has not answered yet. Named because
// it is both written and tested for: a user switch takes its own line down
// again, and must not take the audio watchdog's recovery note with it.
// The note is stored TRANSLATED (the Sinks panel draws audioHealthNote_ as it
// stands), so it is written and compared through tr() in both places.
const char* const kAudioBusyNote = FOX_TR_NOOP("audio device busy - still opening");

// Monotonic milliseconds for the retune coalescer — steady_clock, because a
// wall-clock step (NTP, DST) must never stall or flood the tune pacing.
double steadyNowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch())
        .count();
}

FoxCommandResult commandResultFor(const FoxCommand& c) {
    FoxCommandResult r;
    std::memset(&r, 0, sizeof(r));
    r.structSize = static_cast<std::uint32_t>(sizeof(FoxCommandResult));
    r.status = FOXAPI_OK;
    r.op = c.op;
    return r;
}

// Plugin keys (module file names) carried one per line in a command's text.
std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t to = text.find('\n', from);
        if (to == std::string::npos) { to = text.size(); }
        if (to > from) { out.push_back(text.substr(from, to - from)); }
        from = to + 1;
    }
    return out;
}

// The ops addressed to the open radio itself - the Source panel's device
// controls, the radar scope's gain knob and the bias tee key.
bool isDeviceScoped(std::uint32_t op) {
    switch (op) {
        case FOXAPI_OP_SET_SAMPLE_RATE:
        case FOXAPI_OP_SET_GAIN:
        case FOXAPP_OP_SET_GAIN_NO_READBACK:
        case FOXAPI_OP_SET_DEVICE_AGC:
        case FOXAPI_OP_SET_ANTENNA:
        case FOXAPI_OP_SET_BIAS_TEE:
        case FOXAPI_OP_SET_DEVICE_OPTION:
            return true;
        default:
            return false;
    }
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

void Engine::pollSourceAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    if (soapyScanPending_ && soapyScanFuture_.valid() &&
        soapyScanFuture_.wait_for(kNoWait) == std::future_status::ready) {
        auto found = soapyScanFuture_.get();
        // A SCAN BESIDE AN OPEN RADIO could not see that radio's family, so
        // the rows of the drivers it left out are KEPT from the old list - the
        // open radio's own row among them - and not silently dropped.
        std::vector<cascade::source::SoapyDeviceInfo> kept;
        for (const auto& d : soapyDevices_) {
            if (cascade::gui::rowFromSkippedDriver(soapyScanSkip_, d.args)) { kept.push_back(d); }
        }
        soapyDevices_.clear();
        for (auto& d : found) {
            if (!isAudioDriver(d.args)) { soapyDevices_.push_back(std::move(d)); }
        }
        for (auto& d : kept) {
            const bool present = std::any_of(soapyDevices_.begin(), soapyDevices_.end(),
                                             [&](const cascade::source::SoapyDeviceInfo& x) {
                                                 return x.args == d.args;
                                             });
            if (!present) { soapyDevices_.push_back(std::move(d)); }
        }
        soapyScanSkip_.clear();
        soapyScanPending_ = false;
        // What THIS scan left out for having nothing to find - replacing the
        // last scan's, so the hint follows the list it sits under.
        soapyAbsentDrivers_ =
            soapyScanAbsent_ ? *soapyScanAbsent_ : std::vector<std::string>();
        soapyScanAbsent_.reset();
        if (sourceSel_ >= kNativeRowBase || sourceSel_ < 0) {
            // Re-find the open device by its args (labels can repeat); if it
            // vanished from the scan the device stays open and selected, and
            // the preview falls back to its live name via rowLabel(-1).
            //
            // A NATIVE row is re-found first and by the same rule: the Soapy
            // scan did not touch nativeDevices_, but the row INDEX moves when
            // the Soapy list changes length only for Soapy rows, so a native
            // selection is simply looked up again rather than assumed.
            sourceSel_ = -1;
            for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
                // BY DRIVER AND ARGS (0.99.36): an RSP's native Mirics row and
                // its SDRplay API row carry the same "serial=..." args.
                if (device_ != nullptr && soapyView_ == nullptr &&
                    nativeDevices_[i].driver == sourceKind_ &&
                    nativeDevices_[i].args == deviceArgs_) {
                    sourceSel_ = kNativeRowBase + static_cast<int>(i);
                }
            }
            for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
                if (soapyView_ != nullptr && soapyDevices_[i].args == deviceArgs_) {
                    sourceSel_ = soapyRowBase() + static_cast<int>(i);
                }
            }
        }
    }

    if (deviceOpenPending_ && deviceOpenFuture_.valid() &&
        deviceOpenFuture_.wait_for(kNoWait) == std::future_status::ready) {
        // THE MODULE TABLE, REBUILT BECAUSE THE OPEN LOADED CODE.
        //
        // SoapySDR::Device::make() maps the vendor module and everything it
        // pulls in - rtlsdrSupport.dll, rtlsdr.dll, libusb - into THIS
        // process, on the worker that has just finished. The table a report
        // resolves addresses against was last built at start-up, so until now
        // every one of those modules resolved to a bare address: a field
        // report whose radio was opened 102 s after launch named the fault in
        // hex, and the frame that identified it had to be recovered by hand.
        // diag_report.hpp has always said "after a device is opened"; this is
        // the call that makes that true.
        //
        // BEFORE finishDeviceOpen, not after, because that function returns
        // early on a failed open and on an open the user has moved on from -
        // and both of those loaded the vendor module just the same. Rebuilt
        // once per open, which is a user action, not per frame.
        cascade::core::refreshModuleTable();
        finishDeviceOpen(deviceOpenFuture_.get());
        deviceOpenPending_ = false;
        deviceBusyLabel_.clear();
    }
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

void Engine::finishDeviceOpen(DeviceOpenResult r) {
    // THE USER MAY HAVE MOVED ON. Opening a device takes seconds and the GUI
    // stays live throughout, so by the time this runs they may have selected
    // the generator, opened an IQ file, or done either from the web UI. Before
    // this check the finished open was applied regardless and the radio
    // changed itself several seconds after being told otherwise — including
    // installing a device over a file the user was already listening to.
    //
    // Dropping `r` here also closes the device: r.dev owns the SoapySource,
    // and its destructor is what releases the handle the worker acquired. The
    // error string is dropped with it on purpose — it describes a device the
    // user is no longer asking about, and showing it under the source they DID
    // choose would read as a fault in that source.
    if (!asyncOpenStillWanted(deviceOpenReqGen_, sourceGen_)) { return; }

    // Failure: the reason lands in red under the control. The combo settles
    // on what is actually installed — since the close-first ordering in
    // selectSource, a failed device open leaves the GENERATOR running (the
    // old radio was closed before the attempt), so a selection still pointing
    // at a device row would be a readout disagreeing with the hardware.
    if (!r.dev) {
        const std::string why = r.error.empty() ? "device open failed" : r.error;
        sourceError_ = why;
        if (r.recovery) {
            // THE ONE AUTOMATIC REOPEN DID NOT TAKE. The dead radio was
            // closed before the attempt, the generator is what is installed,
            // and the pipeline's own fault latch still shows "Device stopped"
            // with the driver's reason; nothing tries again
            // (pollSoapyRecovery has no device to reopen now), and the words
            // are the ones the dead-device policy has always used.
            sourceError_ = "the radio could not be reopened after its driver faulted (" +
                           sourceError_ + ") - restart FoxSDR to use this radio again";
            cascade::core::diagWarnf(
                "source: the reopen of %s failed (%s) - restart FoxSDR to use this "
                "radio again",
                cascade::core::sanitiseDevice(r.args).c_str(), r.error.c_str());
        }
        // THE GENERATOR STOOD IN FOR A RADIO - SAY SO, AND KEEP THE RADIO THAT
        // WORKED (0.99.36). The receiver closed the radio it had before this
        // attempt (selectSource's close-first rule), so the generator is what
        // is running. Before 0.99.36 nothing was remembered here and the exit
        // save wrote "siggen": re-opening an RSP whose service had died - which
        // is what the screen then told the user to do - made every later
        // launch start on the generator too. Only when the generator is what
        // is installed: a user who was playing an I/Q file still is.
        if (device_ == nullptr && sourceKind_ == "siggen") {
            if (!restoreKeep_.valid()) {
                restoreKeep_ = cascade::gui::rememberAfterFailedSwitch(restoreKeep_, r.closedRadio);
                if (restoreKeep_.valid()) { restoreKeepLabel_ = r.closedLabel; }
            }
            // The model, never the args, in the log (serial numbers): the
            // same rule as every other source line.
            std::string model = (r.kind == "soapy")
                                    ? cascade::core::sanitiseDevice(r.args)
                                    : modelFromNativeLabel(nativeLabelFor(r.kind, r.args));
            if (model.empty() || model == r.args) { model = r.kind; }
            if (!r.recovery) {
                // Which radio, why, and where the receiver is now - on the
                // screen, under the combo, where the reason already went.
                const std::string wanted = deviceBusyLabel_.empty() ? model : deviceBusyLabel_;
                sourceError_ = cascade::gui::radioNotOpenedSentence(wanted, why);
            }
            cascade::core::diagWarnf(
                "source: %s (%s) did not open - the receiver is on the signal generator%s",
                model.c_str(), r.kind.c_str(),
                restoreKeep_.valid() ? "; the saved radio is kept for the next start" : "");
        }
        // ...and the combo settles on whatever IS installed - unless a saved
        // radio is still being remembered for the config, in which case -1
        // keeps the preview naming that radio instead of ticking a generator
        // nobody chose. Same rule as the restore's failure path.
        // A sound card chosen while this radio was opening keeps its row, as
        // the I/Q file's row does: it is a choice, not a stand-in.
        if (device_ == nullptr && sourceKind_ == "siggen" && sourceSel_ != 1 &&
            sourceSel_ != kSoundCardRow) {
            sourceSel_ = restoreKeep_.valid() ? -1 : 0;
        }
        return;
    }
    // A non-fatal rate refusal still carries its reason.
    if (!r.error.empty()) { sourceError_ = r.error; }

    // Panel mirrors, then gain priming — all quick register writes, unlike
    // the make() that just finished on the worker. One shared function with
    // the synchronous config restore, which used to keep its own copy of this
    // and had already drifted from it.
    adoptDeviceMirrors(*r.dev, r.kind, r.args, r.requestRateHz);
    if (r.recovery) {
        // THE GAINS THE USER HAD, written over the defaults just primed: a
        // reopen after a driver fault is the same radio to the user, and a
        // receiver that came back 20 dB quieter would read as the fault
        // having broken something. By NAME, not by index - the stage list is
        // read back from the device above, and a driver may answer it in a
        // different order than it did last time. The slider mirror follows
        // only a set the driver accepted.
        for (std::size_t i = 0; i < r.recoveryGainNames.size() && i < r.recoveryGainsDb.size();
             ++i) {
            for (std::size_t g = 0; g < deviceGainNames_.size(); ++g) {
                if (deviceGainNames_[g] != r.recoveryGainNames[i]) { continue; }
                if (r.dev->setGainDb(deviceGainNames_[g],
                                     static_cast<double>(r.recoveryGainsDb[i]))) {
                    deviceGainsDb_[g] = r.recoveryGainsDb[i];
                }
            }
        }
        if (r.recoveryAgc && deviceAgcSupported_ && r.dev->setAutoGain(true)) {
            deviceAgc_ = true;
        }
    }
    // The antenna is settled by adoptDeviceMirrors above, on the same rule it
    // always used: apply the saved port if this device has one by that name,
    // then report what the driver actually chose.

    device_ = r.dev.get();
    // ...AND THE SAME OBJECT AS A SoapySource WHEN IT IS ONE. dynamic_cast
    // rather than trusting r.kind: this pointer is what the vendor-fault
    // recovery and the module diagnostics dereference, and a kind string that
    // ever disagreed with the object would make that a wild pointer rather
    // than a wrong caption.
    soapyView_ = dynamic_cast<cascade::source::SoapySource*>(device_);
    deviceArgs_ = r.args;
    deviceModel_ = (r.kind == "soapy") ? cascade::core::sanitiseDevice(r.args)
                                       : modelFromNativeLabel(nativeLabelFor(r.kind, r.args));
    if (r.kind == "soapy") {
        cfgSoapyArgs_ = r.args;
    } else {
        cfgNativeArgs_ = r.args;
    }
    // A RADIO IS OPEN, so there is nothing to remember on its behalf: whatever
    // the startup restore failed to open has just been superseded by a device
    // the user actually has. (A failed open clears nothing - the attempt did
    // not take, and the saved radio is still the best thing to try next time.)
    restoreKeep_ = cascade::gui::RememberedSource{};
    restoreKeepLabel_.clear();
    ++sourceGen_;  // this install is itself a source change
    installSource(std::move(r.dev));
    sourceKind_ = r.kind;
    // A dongle the native driver refused, opened through SoapySDR instead, is
    // still the radio the user chose: its converter comes with it.
    if (!r.fellBackFromKey.empty()) {
        noteConverterFallback(r.fellBackFromKey, cascade::core::converterRadioKey(r.kind, r.args));
    }
    // THIS radio's converter, before the carry-across below tunes it: the air
    // frequency the user was on is sent through the converter in front of the
    // radio now open, not the one in front of the radio just closed.
    applyConverterForSource();
    sourceSel_ = r.row;
    // SERIAL STRIPPED, exactly as everywhere else this string is recorded.
    // "which radio, at what rate" is the single most useful line in the run-up
    // to a fault, because vendor SDR modules are third-party code running
    // in-process and the rate decides whether the chain keeps up. WHICH
    // DRIVER opened it is on the line too now, because there are two ways to
    // reach the same dongle and a report has to say which one was taken.
    cascade::core::diagLogf("source: opened %s (%s) at %.0f S/s", deviceModel_.c_str(),
                            r.kind.c_str(), pipeline_.activeSource().sampleRateHz());

    // CARRY THE FREQUENCY ACROSS, which is the whole difference between
    // changing radio and losing what you were listening to.
    //
    // A freshly opened device sits at its driver's default - an RTL-SDR comes
    // up at 100 MHz - so before this, switching from a B200 tuned to 97 MHz
    // put the receiver on 100 MHz without saying so. The spectrum went empty,
    // the level fell to the noise floor, the squelch stayed shut, and the
    // audio stopped: it reads as "changing device breaks the sound" rather
    // than "your radio is now tuned somewhere else".
    //
    // WHETHER there is a frequency is its own question (keepCenterHz has a
    // value), never a sign: the AIR centre may sit below 0 Hz through an
    // up-converter and still be an ordinary tune for this radio.
    if (r.keepCenterHz.has_value()) {
        const double keepHz = *r.keepCenterHz;
        // A tune the coalescer was still holding was aimed at the OLD source;
        // the carry-across below supersedes it. Applied unpaced, because the
        // readback two lines down must be valid on return. A frequency THIS
        // radio's converter cannot deliver is refused by the pipeline's view
        // without reaching the radio, and noteTuneRefused says why.
        retuneCoalescer_.clearPending();
        applyRetuneNow(keepHz);
        // Not every radio covers every band, so say so rather than leaving
        // the user on a frequency they did not choose. The readback is the
        // authority - a device may clamp to its range or land on a nearby
        // tuning step. (An unreachable one has the converter's sentence.)
        const double landed = pipeline_.activeSource().centerFrequencyHz();
        if (cascade::core::airReachable(pipeline_.converter(), keepHz) &&
            std::fabs(landed - keepHz) > 1000.0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "this radio could not tune %.6f MHz; it is on %.6f MHz",
                          keepHz / 1e6, landed / 1e6);
            sourceError_ = buf;
        }
    }

    followInputRate();  // DSP chain follows the device's actual readback

    if (r.recovery) {
        // THE RECEIVER GOES BACK TO WHAT IT WAS DOING. It was running when the
        // driver faulted (or the pipeline noticed the fault, which only its
        // running source thread can) - so it runs again; Pipeline::start
        // clears the fault latch the source thread raised, which is what
        // takes "Device stopped" and the FAIL lamp off the deck. A receiver
        // that was stopped stays stopped: the reopen is a repair, not a
        // Play press.
        if (r.recoveryRestart) { startReceiver(); }
        cascade::core::diagLogf("source: reopened %s at %.0f S/s after the driver fault%s",
                                deviceModel_.c_str(),
                                pipeline_.activeSource().sampleRateHz(),
                                r.recoveryRestart ? "; receiver restarted" : "");
    }
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

void Engine::pollSoapyRecovery() {
    // Cheap on every frame that matters: no device, or a live one, and this
    // is two loads. Every bounded --frames run takes the first return.
    //
    // SOAPY ONLY, THROUGH soapyView_, and deliberately so. What this repairs
    // is a VENDOR call that faulted on our own call frame and was absorbed by
    // the structured-exception guard, which SoapySource::deadReason() can
    // distinguish from a driver left wedged with one of our threads inside
    // it. A native driver has neither: its faults are returned, not raised,
    // and a native radio that has gone dead has done so for a reason its own
    // lastError() already states. Reopening one on a timer would be guessing.
    if (soapyView_ == nullptr || !soapyView_->deviceDead()) { return; }
    using DeadReason = cascade::source::SoapySource::DeadReason;
    const DeadReason reason = soapyView_->deadReason();
    const double now = host_.frameTimeS();
    if (!cascade::gui::autoReopenDue(reason == DeadReason::VendorFault,
                                     reason == DeadReason::Abandoned, deviceOpenPending_,
                                     soapyScanPending_, now, soapyReopenAttemptSec_)) {
        return;
    }
    // Stamped BEFORE anything can fail, so a reopen that faults again (which
    // condemns its own device the same way) is held off for the minute
    // rather than tried again on the next frame.
    soapyReopenAttemptSec_ = now;
    reopenAfterDriverFault();
}

void Engine::reopenAfterDriverFault() {
    // What pollSoapyRecovery decided is due. Everything below reads the dead
    // radio through device_ - the same object as soapyView_ whenever that
    // gate let it through - so the reopen itself is DeviceSource work and a
    // test can drive it with a device of its own (AppWindowTestAccess).
    if (device_ == nullptr) { return; }

    // EVERYTHING THE REOPEN NEEDS IS READ FROM THE DEAD SOURCE FIRST. Its
    // mirrors survive the fault on purpose (a rate or a retune that faulted
    // leaves the last CONFIRMED readback in place, never the request), and
    // the close below is what clears them.
    DeviceOpenResult r;
    r.kind = "soapy";
    r.args = deviceArgs_;
    r.row = sourceSel_;
    const double confirmedRateHz = device_->sampleRateHz();
    r.requestRateHz = confirmedRateHz > 0.0 ? confirmedRateHz
                                            : kSoapyRateHz[kSoapyRateDefaultIndex];
    // Through the pipeline, not the Soapy object: the device IS the active
    // source here, and only the pipeline's view speaks the AIR frequency the
    // reopen carries across (the same one it will be sent through again) -
    // which may be below 0 Hz on the air and is still a frequency.
    r.keepCenterHz = carriedAirCentre();
    r.recovery = true;
    r.recoveryGainNames = deviceGainNames_;
    r.recoveryGainsDb = deviceGainsDb_;
    r.recoveryAgc = deviceAgc_;
    // Running, or faulted - the pipeline's latch is raised only by its own
    // source thread, which exists only while the receiver runs, so a latched
    // fault is proof it was running when the driver went.
    r.recoveryRestart = pipeline_.running() || pipeline_.faulted();
    std::string what = device_->faultedWhile();
    if (what.empty()) { what = "a driver call"; }
    std::string label = pipeline_.activeSource().name();
    const std::size_t colon = label.rfind(": ");
    if (colon != std::string::npos) { label = label.substr(colon + 2); }
    cascade::core::diagLogf("source: the driver faulted while %s; reopening %s at %.0f S/s",
                            what.c_str(), cascade::core::sanitiseDevice(r.args).c_str(),
                            r.requestRateHz);
    // Remembered if the reopen fails (0.99.36), so the exit save does not
    // write the generator over a radio that only needed a restart.
    r.closedRadio = cascade::gui::rememberedSourceAfterFailedOpen(
        sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, "", r.requestRateHz);
    r.closedLabel = deviceModel_;

    // CLOSE THE DEAD RADIO BEFORE OPENING IT AGAIN - the same order as
    // selectSource (adjudicated fix #3 for the 0.62.0 field crashes: two
    // device lifetimes must not overlap in one process's libusb). The close
    // makes no driver call at all - the dead-device policy drops a faulted
    // handle without closeStream or unmake - so what this costs is the
    // generator running until the open resolves, exactly as a device switch
    // costs it. WHAT THE REOPEN THEN REACHES: SoapySDR's factory keeps every
    // made device in a table keyed by its enumerated args and hands the SAME
    // object back for the same args (lib/Factory.cpp, getDeviceFromTable),
    // and the abandoned device was never unmade, so Device::make answers
    // from the table without running the vendor's find at all - no probe, no
    // dongle reset, and the driver's own object is asked to set up a fresh
    // stream. Whether it can is the driver's answer to give: it is made
    // under the same guard as every open, and a fault or a refusal there is
    // the ordinary failed open, reported the ordinary way.
    //
    // BOTH TAKES END FIRST. The reopen restarts the receiver on its own, and
    // a recording carried across it would splice the stretch before the
    // fault onto whatever the reopened radio hears, with a gap in between
    // that the file cannot show. endTakesOnFault has usually done this
    // already, on the frame the pipeline's latch rose; this is for a radio
    // declared dead without the pipeline's source thread raising it.
    endTakes(true, true, "the radio faulted", true);
    device_ = nullptr;
    soapyView_ = nullptr;
    deviceArgs_.clear();
    deviceModel_.clear();
    ++sourceGen_;
    installSource(nullptr);
    sourceKind_ = "siggen";
    applyConverterForSource();
    followInputRate();
    launchDeviceOpen(std::move(r), label);
}

void Engine::openPlutoFromBox() {
    // The row's args are re-derived from the box here rather than read out
    // of nativeDevices_, because the user may have typed since the list was
    // built. What the Open key submits (SELECT_SOURCE "open-pluto:<args>");
    // kept as a member so the converter test can press it.
    openPlutoAt(std::string("uri=") + plutoUri_);
}

void Engine::openPlutoAt(const std::string& args) {
    sourceError_.clear();
    // Through the SAME worker-thread open every other radio uses, so a board
    // that is not there spends its connect bound off the GUI thread and the
    // window keeps drawing.
    const std::string uri = (args.rfind("uri=", 0) == 0) ? args.substr(4) : args;
    // The frequency to carry, read BEFORE the close below: after it the
    // generator is what answers.
    const std::optional<double> keepCenterHz = carriedAirCentre();
    DeviceOpenResult req;
    if (device_ != nullptr) {
        // Carried with the request, as selectSource does (0.99.36).
        req.closedRadio = cascade::gui::rememberedSourceAfterFailedOpen(
            sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, "",
            pipeline_.activeSource().sampleRateHz());
        req.closedLabel = deviceModel_;
        cascade::core::diagLogf("source: closing %s before opening the ADALM-Pluto",
                                deviceModel_.c_str());
        device_ = nullptr;
        soapyView_ = nullptr;
        deviceArgs_.clear();
        deviceModel_.clear();
        ++sourceGen_;
        installSource(nullptr);
        sourceKind_ = "siggen";
        applyConverterForSource();
        followInputRate();
    }
    req.kind = kPlutoDriverKey;
    req.args = args;
    req.row = sourceSel_;
    req.requestRateHz = kSoapyRateHz[kSoapyRateDefaultIndex];
    req.keepCenterHz = keepCenterHz;
    launchDeviceOpen(std::move(req), "ADALM-Pluto at " + uri);
}

void Engine::selectSource(int idx, std::optional<double> carryAirHz) {
    // RE-CLICK ON THE CURRENT ROW: a no-op - unless the radio installed there
    // has DIED (unplugged, or a driver fault the receiver latched), when the
    // screen says "Reconnect it and pick the source again" and picking it
    // must do exactly that: close the dead radio and open it again (0.99.36,
    // the 0c59853 review; gui::pickOpensRow). Every family, not only an RSP.
    // A SOUND CARD that has stopped is the same promise, on its own row (it
    // is never device_): the row picked again opens the card again.
    const bool installedCardDead = installedSoundCardDead();
    const bool installedDead =
        (device_ != nullptr && (pipeline_.faulted() || device_->deviceDead())) || installedCardDead;
    if (!cascade::gui::pickOpensRow(idx, sourceSel_, installedDead)) { return; }

    // BUSY CHECK ON EVERY ROW, not just the device rows (adjudicated fix #4
    // for the 0.62.0 field crashes: selectSource(0) and the web route lacked
    // the check the idx>=2 path had). While a device open is resolving on its
    // worker, no source switch of any kind is accepted — switching to the
    // generator mid-open used to strand the resolving device for a stale-drop
    // teardown, and the combo's busy label already tells the user why the
    // click did nothing.
    if (deviceOpenPending_) { return; }
    sourceError_.clear();

    if (idx == 0) {
        // A DELIBERATE CHOICE OF THE GENERATOR STILL OVERWRITES THE SAVED
        // RADIO. The remembered-source rule holds a radio a RESTORE could not
        // open; a user picking this row is saying they want the generator, and
        // the config has to be able to say so too. Cleared here rather than in
        // sourceToSave because only this side knows the difference between
        // "the generator stood in" and "the generator was chosen".
        restoreKeep_ = cascade::gui::RememberedSource{};
        restoreKeepLabel_.clear();

        // Built-in generator: null restores it, and it cannot fail.
        device_ = nullptr;  // before setSource destroys a live device
        soapyView_ = nullptr;
        deviceArgs_.clear();
        deviceModel_.clear();
        ++sourceGen_;  // a device open still in flight is now stale
        installSource(nullptr);
        sourceKind_ = "siggen";
        applyConverterForSource();
        sourceSel_ = 0;
        followInputRate();  // back to the generator's fixed 2 MS/s
        cascade::core::diagLogf("source: switched to the built-in generator");
        return;
    }
    if (idx == 1) {
        // Show the path controls only; the switch happens on a successful
        // Open (see drawSourceSection) so a typo can never kill a live source.
        sourceSel_ = 1;
        return;
    }
    if (idx == kSoundCardRow) {
        // The same for the sound card: its controls, and the list of inputs
        // asked for on a worker the first time. Nothing opens until Open -
        // EXCEPT the installed card when it has died: picking its row again
        // reopens it exactly as it was running (soundCardLive_), the same
        // as picking a dead radio's row (gui::pickOpensRow). A card that
        // was taken by another program, or stalled, can come back this way;
        // one that was unplugged usually cannot until FoxSDR restarts
        // (PortAudio lists inputs once per session - the card's own fault
        // sentence says so), and the open's failure is then said as any
        // other. The dead card is released first (launchSoundCardOpen, same
        // card) and not waited for, and tried once (its settings are the
        // ones it ran with). ON LINUX it is not reopened at all: an ALSA
        // entry opens by card number, which another card plugged in since
        // may now have, so launchSoundCardOpen refuses with the restart
        // sentence instead (gui::soundCardDeadReopenNeedsRestart).
        sourceSel_ = kSoundCardRow;
        if (installedCardDead) {
            cascade::core::diagLogf("source: reopening the sound card %s (%s), which had stopped",
                                    soundCardLive_.device.c_str(), soundCardLive_.hostApi.c_str());
            soundCard_ = soundCardLive_;
            launchSoundCardOpen(false, soundCardLive_);
            return;
        }
        if (!soundCardListed_) { scanSoundCards(); }
        return;
    }

    // WHICH FAMILY THE ROW BELONGS TO. Native rows come first (see rowLabel);
    // anything past them is a SoapySDR device.
    std::string kind;
    std::string args;
    std::string label;
    std::string fallbackSoapyArgs;
    if (idx < soapyRowBase()) {
        const std::size_t n = static_cast<std::size_t>(idx - kNativeRowBase);
        if (n >= nativeDevices_.size()) { return; }  // stale row; next frame redraws
        kind = nativeDevices_[n].driver;
        args = nativeDevices_[n].args;
        label = nativeDevices_[n].label;
        // THE PLUTO ROW SELECTS AND DOES NOT CONNECT, exactly as the IQ file
        // row selects and does not open. Every other row in this list is a
        // radio that was FOUND, so choosing it can only succeed or fail
        // quickly; this one is an address that has never been contacted, and
        // opening it on selection would spend the driver's connect bound in
        // front of a user who was only reading the list. The address box and
        // the Open key appear instead (see drawSourceSection), and the
        // pipeline keeps whatever is installed until Open succeeds.
        if (kind == kPlutoDriverKey) {
            sourceSel_ = idx;
            return;
        }
    } else {
        const std::size_t d = static_cast<std::size_t>(idx - soapyRowBase());
        if (d >= soapyDevices_.size()) { return; }  // stale row; next frame redraws
        if (soapyScanPending_) { return; }  // one at a time (open is checked above)
        kind = "soapy";
        args = soapyDevices_[d].args;
        label = soapyDevices_[d].label;
        // PICKING THE SOAPY ROW FOR A DONGLE WE DRIVE OURSELVES OPENS IT
        // NATIVELY. Two rows can name one physical radio - SoapyRTLSDR's and
        // ours - and choosing a radio should not also be choosing which of
        // two code paths reaches it. The user gets the one this product can
        // be held responsible for, and the log says the swap happened.
        if (const std::optional<cascade::source::NativeDeviceInfo> nat =
                cascade::gui::preferNativeFor("soapy", args, nativeDevices_)) {
            cascade::core::diagLogf(
                "source: opening %s natively (was SoapySDR %s)",
                modelFromNativeLabel(nat->label).c_str(),
                cascade::core::sanitiseDevice(args).c_str());
            fallbackSoapyArgs = args;
            kind = nat->driver;
            args = nat->args;
            label = nat->label + " (native)";
        }
    }
    // The open runs on a worker: Device::make() is the multi-second, USB-bus
    // -walking call that used to freeze the GUI here, and a native open is a
    // full demodulator and tuner initialisation. sourceSel_ is left alone
    // until it resolves; on failure finishDeviceOpen settles the combo on
    // whatever is actually installed.
    const double rate = kSoapyRateHz[kSoapyRateDefaultIndex];
    // Read the tuned frequency NOW: the old source is closed below. A device
    // that has never been tuned has nothing to carry (carriedAirCentre); a
    // caller that knows better - the patch page handing a radio back - says
    // what to carry instead.
    const std::optional<double> keepCenterHz =
        carryAirHz.has_value() ? carryAirHz : carriedAirCentre();

    // CLOSE THE OLD RADIO BEFORE OPENING THE NEW ONE (adjudicated fix #3 for
    // the 0.62.0 field crashes). The previous flow opened the new device on
    // the worker while the old one was still open and streaming, then unmade
    // the old one afterwards — two device lifetimes overlapping in one
    // process's libusb, which is the lifecycle overlap fingerprinted as the
    // corrupting event. Now: quiesce and destroy the old device HERE, on this
    // thread, with the source thread joined (setSource does both), and only
    // then let the worker call Device::make. The cost is honest: if the new
    // device fails to open, the receiver is on the generator with the reason
    // shown, not silently back on a radio it had to close to try.
    //
    // ...AND THE CLOSED RADIO IS CARRIED WITH THE REQUEST (0.99.36), exactly
    // as the exit save would have named it, so a failed open leaves the
    // config naming the radio that worked instead of the generator.
    cascade::gui::RememberedSource closedRadio;
    std::string closedLabel;
    if (device_ != nullptr) {
        closedRadio = cascade::gui::rememberedSourceAfterFailedOpen(
            sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, "", pipeline_.activeSource().sampleRateHz());
        closedLabel = deviceModel_;
        cascade::core::diagLogf("source: closing %s before opening another device",
                                deviceModel_.c_str());
        device_ = nullptr;
        soapyView_ = nullptr;
        deviceArgs_.clear();
        deviceModel_.clear();
        ++sourceGen_;
        installSource(nullptr);
        sourceKind_ = "siggen";
        applyConverterForSource();
        // The DSP chain must follow the source that is actually installed —
        // if the open below fails, the generator would otherwise keep running
        // at the closed radio's rate.
        followInputRate();
    }
    DeviceOpenResult req;
    req.kind = kind;
    req.args = args;
    req.fallbackSoapyArgs = fallbackSoapyArgs;
    req.row = idx;
    req.requestRateHz = rate;
    req.keepCenterHz = keepCenterHz;
    req.closedRadio = std::move(closedRadio);
    req.closedLabel = std::move(closedLabel);
    launchDeviceOpen(std::move(req), label);
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

void Engine::scanNative() {
    // NO GATE, and that is the whole point of having our own transport. All
    // six USB enumerations read SetupAPI device properties and
    // never open a device, never send a transfer, never reset anything -
    // usb_device.hpp rule 1, which exists precisely because the SoapySDR
    // vendor probe breaks it and killed a running capture doing so (the
    // 0.90.0 field report behind gui::deviceScanAllowed). So this runs on the
    // GUI thread, inline, whenever the list might be stale, including while a
    // radio of ours is streaming.
    //
    // ...AND THE SELECTION FOLLOWS THE RADIO, NOT THE INDEX (0.99.36). The
    // combo's selection is a row number, and this rebuilds the rows; one
    // that appears or vanishes moves every row after it. Keyed now, found
    // again at the end.
    const std::vector<cascade::gui::SourceRowKey> rowsBefore = sourceRowKeys();
    if (testHooks_.nativeScan != nullptr) {
        // The test's list stands in for the USB walk (see testHooks_): no
        // enumeration of any kind runs, and nothing is reported unbound. The
        // selection is followed exactly as after a real scan, so a test can
        // move rows under it.
        nativeDevices_ = testHooks_.nativeScan();
        nativeRowLabels_.clear();
        for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
            nativeRowLabels_.push_back(d.label);
        }
        nativeUnbound_.clear();
        followSourceRowAfterRescan(rowsBefore);
        return;
    }
    nativeDevices_ = cascade::source::enumerateRtlSdr();
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateHackRf()) {
        nativeDevices_.push_back(std::move(d));
    }
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateAirspy()) {
        nativeDevices_.push_back(std::move(d));
    }
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateAirspyHf()) {
        nativeDevices_.push_back(std::move(d));
    }
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateMiriSdr()) {
        nativeDevices_.push_back(std::move(d));
    }
    // BOTH RX888 IDENTITIES, and the bootloader is listed rather than hidden.
    // Out of a power cycle an RX888 has no firmware and is a Cypress
    // bootloader with no bulk endpoint at all; its row says so ("needs
    // firmware, will load on open") and opening it uploads the image. Hiding
    // it would leave a plugged-in radio missing from the list with nothing
    // anywhere saying why - the same failure the unbound-device sentence
    // below exists to stop.
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateRx888()) {
        nativeDevices_.push_back(std::move(d));
    }
    // THE ONE ENUMERATION THAT IS NOT A USB WALK AND IS STILL SAFE HERE.
    // sdrplay_api_GetDevices asks the SDRplay SERVICE for a list it maintains
    // anyway; it never touches the bus, so rule 1's reason does not apply to
    // it and it can run while a radio of ours is streaming, exactly like the
    // others. On a machine with no SDRplay install it costs one failed
    // LoadLibrary, cached for the life of the process.
    const std::size_t beforeSdrPlay = nativeDevices_.size();
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateSdrPlay()) {
        sdrPlaySeenLabels_[d.args] = d.label;
        nativeDevices_.push_back(std::move(d));
    }
    // THE RSP THIS PROCESS HAS OPEN IS NOT IN THAT LIST (0.99.36). The API
    // lists the radios free to select, and a selected one is not free - the
    // reference module puts "the cached results for claimed handles" back
    // after every GetDevices for exactly this reason. Without it, opening the
    // Source combo while an RSP played dropped the RSP's row. Claimed here:
    // the receiver's radio and every patch radio opened through the API.
    {
        std::vector<cascade::source::NativeDeviceInfo> claimed;
        const auto claim = [&](const std::string& args, const std::string& fallbackName) {
            cascade::source::NativeDeviceInfo c;
            c.driver = "sdrplay";
            c.args = args;
            const auto seen = sdrPlaySeenLabels_.find(args);
            c.label = (seen != sdrPlaySeenLabels_.end()) ? seen->second : fallbackName;
            claimed.push_back(std::move(c));
        };
        if (device_ != nullptr && soapyView_ == nullptr && sourceKind_ == "sdrplay" &&
            !deviceArgs_.empty()) {
            claim(deviceArgs_, pipeline_.activeSourceName());
        }
        for (const auto& [id, as] : patchRadioOpenedAs_) {
            if (patchRadios_.find(id) == patchRadios_.end()) { continue; }
            const std::string key = as.substr(0, as.rfind('@'));
            if (cascade::core::patch::deviceDriver(key) != "sdrplay") { continue; }
            claim(cascade::core::patch::deviceArgs(key), "SDRplay");
        }
        nativeDevices_ = cascade::source::withClaimedSdrPlayRows(nativeDevices_, claimed);
    }
    sdrPlayRowsFound_ = nativeDevices_.size() > beforeSdrPlay;

    // ONE RADIO, ONE ROW (0.99.9, at the owner's word). An RSP reachable both
    // natively and through the SDRplay API was offered TWICE, under two names,
    // with nothing saying which to pick - and picking the native one gets the
    // less capable of the two, because the API drives front-end hardware this
    // application cannot. source/rsp_rows.hpp holds the rule and says why the
    // hiding is as narrow as it is: a row hidden in error is a radio the user
    // cannot select at all.
    {
        const std::size_t before = nativeDevices_.size();
        nativeDevices_ = cascade::source::withoutDuplicateRsps(nativeDevices_);
        const std::size_t hidden = before - nativeDevices_.size();
        if (hidden > 0) {
            // LOGGED, because a row that vanishes without explanation is the
            // fault report this is trying to prevent, arriving from the other
            // direction.
            cascade::core::diagLogf(
                "source: %zu native SDRplay row(s) hidden - the SDRplay API already lists "
                "that radio, and it is the fuller driver of the two",
                hidden);
        }
    }
    // ...AND WHAT TO SAY WHEN THERE IS NO RSP ROW BECAUSE THERE IS NO API.
    // Composed here rather than in the draw, because the draw runs sixty
    // times a second and this reads the process's load result. The sentence
    // itself comes from the driver (sdrPlayPanelAdvice), which is pure and
    // pinned by a test: it is the only instruction an RSP owner gets, and a
    // rewording that drops "3.x" or "sdrplay.com" sends them nowhere.
    //
    // THROUGH THE ENUMERATION'S OWN REASON, because the two fields below
    // cannot describe an API THAT IS INSTALLED BUT TOO OLD. `version` is
    // written onto the table only by a session that got past the version
    // gate, so a 3.05 install leaves it at zero, and asking the load result
    // alone gave an RSP owner an empty Source section while the log carried
    // the sentence telling them to update. The enumeration a few lines up has
    // just recorded it; sdrPlayPanelAdvice prefers that and falls back to the
    // load result, which is what this used to do on its own.
    const cascade::source::sdrplay_abi::Api& sdrApi = cascade::source::processSdrPlayApi();
    float sdrVersion = 0.0f;
    {
        std::lock_guard<std::mutex> lk(sdrApi.sessionMutex);
        sdrVersion = sdrApi.version;
    }
    sdrPlayAdvice_ = cascade::source::sdrPlayPanelAdvice(
        sdrApi.resolved, sdrVersion, cascade::source::sdrPlayLastEnumerationSkip());
    sdrPlayApiDetail_ = sdrApi.loadDetail;

    // THE PLUTO, WHICH IS NOT A DISCOVERY AT ALL. A network cannot be walked,
    // so there is no honest way to answer "is there a Pluto out there" -
    // every other row in this list means "this radio is plugged into this
    // machine", and a probe to find one would break rule 1's spirit by
    // opening something to ask.
    //
    // ONE ROW, ALWAYS, AT THE END, and it opens nothing when it is chosen.
    // The driver's own enumeratePluto() offers two addresses unconditionally,
    // which would put two permanent rows in front of every user who has never
    // owned a Pluto; this is one row that says what it is, and choosing it
    // shows an address box and an Open key instead of connecting. Last in the
    // list because the rows above it are radios that are really there.
    {
        cascade::source::NativeDeviceInfo pluto;
        pluto.driver = kPlutoDriverKey;
        pluto.label = FOX_TR_NOOP("ADALM-Pluto (network)");
        // The args the Open key will use, kept in step with the box so that a
        // restored Pluto's saved nativeArgs matches this row and the combo
        // settles on it (see applyConfig).
        pluto.args = std::string("uri=") + plutoUri_;
        nativeDevices_.push_back(std::move(pluto));
    }
    nativeRowLabels_.clear();
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // "(native)" is not decoration: with a Soapy row for the same dongle
        // two lines below it, the user has to be able to see which one they
        // are picking, and which one they got. The Pluto's row is exempt: it
        // is not a radio that was found, and "(native)" on it would suggest
        // there is a non-native row for the same board somewhere.
        if (d.driver == kPlutoDriverKey) {
            nativeRowLabels_.push_back(tr(d.label.c_str()));  // FOX_TR_NOOP where it is set
        } else {
            std::string rowBuf;
            cascade::core::formatUtf8(rowBuf, tr("%s (native)"), d.label.c_str());
            nativeRowLabels_.push_back(rowBuf);
        }
    }
    // ...and the radios that are HERE BUT UNREACHABLE. Listing them as rows
    // would offer an open that cannot succeed; the section says what to do
    // about them instead (see drawSourceSection).
    //
    // EVERY USB FAMILY WE DRIVE GOES IN THIS QUERY, not just the dongles. An
    // Airspy ships on its own vendor driver just as an RTL dongle ships on
    // the DVB-T one, and an owner who has not run Zadig on it would otherwise
    // see nothing at all in the list and be told nothing about why. The
    // Mirics family is the worst case of all: most of those devices are
    // television sticks that arrive running a DVB-T driver, so an unbound one
    // is the NORMAL state of a stick somebody has just plugged in.
    //
    // BOTH RX888 IDS GO IN, and that is the one entry here that is not
    // symmetrical with the rows above. An RX888 has two USB identities -
    // 04B4:00F3 before its firmware and 04B4:00F1 after - and each has to be
    // bound to WinUSB separately, so a user who has done only one of them has
    // a radio that is half reachable. Listing whichever identity is currently
    // unbound is what lets the sentence below say which one.
    std::vector<cascade::usb::UsbId> ids = cascade::source::rtlSdrUsbIds();
    for (const cascade::usb::UsbId& id : cascade::source::hackRfUsbIds()) {
        ids.push_back(id);
    }
    for (const cascade::usb::UsbId& id : cascade::source::airspyUsbIds()) {
        ids.push_back(id);
    }
    for (const cascade::usb::UsbId& id : cascade::source::airspyHfUsbIds()) {
        ids.push_back(id);
    }
    for (const cascade::usb::UsbId& id : cascade::source::msi2500::usbIds()) {
        ids.push_back(id);
    }
    for (const cascade::usb::UsbId& id : cascade::source::rx888UsbIds()) {
        ids.push_back(id);
    }
    nativeUnbound_ = cascade::usb::enumerateUnbound(ids);

    followSourceRowAfterRescan(rowsBefore);
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

void Engine::followInputRate() {
    const double rate = pipeline_.activeSource().sampleRateHz();
    if (!(rate > 0.0)) { return; }  // never-opened source; nothing to follow
    // A refusal (the chain kept its old rate: no decimation gives an exact
    // channel, or out of the supported range) is said on the device panel,
    // because the display span then reflects the OLD rate, which is exactly
    // what the DSP is still doing. The next ACCEPTED rate takes that line away
    // again - through 0.99.29 nothing did, so going back to a working rate
    // left "refused" on screen - and leaves any device error on it alone.
    const bool accepted = pipeline_.setInputRateHz(rate);
    sourceError_ = cascade::gui::sourceErrorAfterRateFollow(sourceError_, accepted, rate,
                                                            pipeline_.inputRateHz());
    // An ACCEPTED rate change finalizes an in-flight IQ take: the WAV header
    // rate is fixed at start(), so recording on across a rate switch would
    // produce a file that replays detuned/off-speed. (A refusal above kept
    // the old rate, so the compare — not the call — decides.) The audio take
    // is untouched: its 48 kHz output rate survives every rate switch.
    if (iqRecorder_.recording() && pipeline_.inputRateHz() != iqRecordRateHz_) {
        stopIqRecording();
    }

    // Every source change funnels through here, which makes it the one place
    // that can keep the decoder instances honest about the rate and centre
    // they were built for. Cheap when no plugins are loaded, and it must run
    // even when the rate was REFUSED above: the centre frequency has usually
    // moved regardless, and a decoder told the wrong one reports confidently
    // wrong things.
    refreshPluginRunner();
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

void Engine::rescanPlugins() {
    // BLOCKING WORK THE APPLICATION ENTERS KNOWINGLY, and the one such path
    // this application actually has on its GUI thread: unloading and then
    // LoadLibrary-ing every installed plugin, off a disk that may be cold, a
    // network profile, or a drive that is spinning up. Twelve modules is a
    // normal count. That is a legitimate multi-second gap in the frame loop
    // and it is not a hang, so the watchdog is paused across it — which is
    // also what keeps the watchdog from suspending a thread that is inside the
    // loader (see the phase-1 note in hang_watchdog.cpp).
    //
    // Scope guard, because there is a `return` in the middle of this function.
    HostWatchdogPause holdWatchdog(host_);

    // The plugin windows the user has OPEN ride through the rescan: their
    // identities are the plugin's name and its window's title, so a plugin
    // that survives the rescan keeps its window on screen and one that does
    // not simply has no window to show. (Until 0.79.1 this cleared the set
    // of CLOSED windows, which was then the only way to get one back; a
    // window now opens from its own row on the rail instead.)

    // The map pages are NOT cleared. The first version cleared them here,
    // and an adversarial review measured the cost: every MapView died, so a
    // rescan — which also fires from the plugin store's async catalogue fetch
    // and from installs, seconds after an unrelated click — silently reset
    // zoom, centre, selection and an active follow. Geometry is folded into
    // the saved store as a belt (a page whose plugin vanishes is pruned in
    // drawPluginWindows, and that prune re-syncs first), and the page
    // objects and their views all ride through the rescan untouched. A
    // plugin that reappears finds its page exactly
    // where it was.
    host_.beforePluginRescan();

    // A missing plugins directory is the normal case and yields an empty list
    // without an error — the host's documented behaviour, and the reason
    // nothing here reports a failure.
    pluginDir_ = (testHooks_.pluginDir != nullptr) ? testHooks_.pluginDir()
                                                   : cascade::core::PluginHost::defaultPluginDir();

    // WHERE, not just WHAT. The lines below say which modules loaded; none of
    // them said which of the two candidate directories they came from, and
    // that is the question every "my plugin vanished after an upgrade" report
    // actually asks - beside the exe for a portable or per-user install,
    // %LOCALAPPDATA%\foxsdr\plugins for a Program Files one or for a Store
    // package. One line, once per scan, naming the directory and the reason.
    if (cascade::core::runningInPackage()) {
        cascade::core::diagLogf("plugins: %s (this is a Store package: %s)", pluginDir_.c_str(),
                                cascade::core::packageIdentity().fullName.c_str());
    } else {
        cascade::core::diagLogf("plugins: %s", pluginDir_.c_str());
    }

    // ONE ordered sequence, and the order is the feature (see the enforcement
    // note in app_window.hpp). Nothing may hold a mapped module while files
    // are renamed, so the unload comes first — the same unload-then-touch-the
    // -file rule removeInstalledPlugin already follows.
    //
    // The RUNNER comes off before any of that, and in two steps. Detaching it
    // from the pipeline stops the DSP thread reaching it; clearing it then
    // destroys the decoder instances. Both must complete before unloadAll(),
    // because a live handle is memory inside a module that is about to be
    // unmapped, and destroy() is code inside that same module. Getting this
    // order wrong is a crash in someone else's DLL with no useful stack.
    detachAndUnloadPlugins();
    pluginEnforceError_.clear();

    // Un-quarantine BEFORE taking the inventory. Reconciliation and
    // planUpdates both key off "is the file there", and a plugin that this
    // code renamed aside last frame would otherwise read as deleted by the
    // user — which planUpdates deliberately refuses to update, taking away the
    // one remedy a retired plugin has.
    std::string restoreError;
    if (!restoreQuarantinedPlugins(restoreError)) { pluginEnforceError_ = restoreError; }

    std::string invError;
    // A corrupt or absent manifest is an ordinary state and its own kind of
    // fail-open: nothing is recorded, so nothing is retired.
    (void)cascade::core::PluginRepo::loadInventory(pluginDir_, pluginInventory_, invError);
    // WHAT RECONCILIATION FOUND, one line each - above all a file whose bytes
    // are not the ones sha256-verified at install, which is a manual overwrite
    // or something worse. The fitted modules plate says it too
    // (changedSinceInstallNote); this is the record that outlives the window.
    for (const std::string& note : pluginInventory_.notes) {
        cascade::core::diagLogf("plugin inventory: %s", note.c_str());
    }
    pluginBlocked_ = cascade::core::PluginRepo::blockedPlugins(pluginInventory_.plugins,
                                                               pluginInventory_.policies);

    std::string quarantineError;
    if (!quarantineBlockedPlugins(quarantineError)) {
        // FAIL CLOSED. Scanning now would map the retired plugin along with
        // everything else, which is the precise state this feature exists to
        // prevent, so this run gets no plugins at all and says why.
        pluginEnforceError_ = quarantineError;
        return;
    }

    pluginHost_.scan(pluginDir_);

    // WHICH PLUGINS ARE MAPPED, one line each, because plugins are
    // third-party code running in this process and "which one was loaded" has
    // already been the answer to real faults here. A load that FAILS is worth
    // more than one that succeeds, so both are recorded.
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded) {
            cascade::core::diagLogf("plugin: loaded %s %s", p.name.c_str(),
                                    p.version.c_str());
        } else {
            // A refused plugin has no descriptor, so name and version are
            // empty - the FILE is the only thing that identifies it.
            const std::string leaf =
                std::filesystem::path(p.path).filename().string();
            cascade::core::diagWarnf("plugin: NOT loaded %s (%s)", leaf.c_str(),
                                     p.error.empty() ? "no reason given" : p.error.c_str());
        }
    }
    // The module table has just changed - new code is mapped, and a fault
    // inside it would otherwise resolve to "?" with a bare address.
    cascade::core::refreshModuleTable();

    // Instances are created only after the scan has settled, and the pipeline
    // is only pointed at the runner once they exist — so the DSP thread never
    // sees a half-built set.
    refreshPluginRunner();
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

void Engine::pollPluginAsync() {
    constexpr auto kNoWait = std::chrono::seconds(0);

    if (catalogPending_ && catalogFuture_.valid() &&
        catalogFuture_.wait_for(kNoWait) == std::future_status::ready) {
        CatalogFetchResult r = catalogFuture_.get();
        catalogPending_ = false;
        if (r.ok) {
            catalog_ = std::move(r.entries);
            std::string statusBuf;
            cascade::core::formatUtf8(statusBuf,
                          catalog_.size() == 1u ? tr("%zu plugin in the catalogue")
                                                : tr("%zu plugins in the catalogue"),
                          catalog_.size());
            catalogStatus_ = statusBuf;
            // "When the user last chose to look" — recorded here, never acted
            // on: no code path reads this to decide whether to fetch (see
            // AppConfig::pluginLastUpdateCheck).
            pluginLastUpdateCheck_ = static_cast<std::int64_t>(std::time(nullptr));
            // The floors the worker just cached only take effect on the next
            // scan, and a user who has just been told a plugin is retired
            // should not have to restart to stop running it.
            rescanPlugins();
            // A cache-write failure is red text next to a catalogue that
            // nevertheless loaded: the list is real, the policy behind it was
            // not remembered, and both facts are shown.
            if (!r.policyError.empty()) { catalogError_ = r.policyError; }
        } else {
            catalog_.clear();
            catalogError_ = r.error;
        }
        host_.onCatalogueResult();
    }

    if (installPending_ && installFuture_.valid() &&
        installFuture_.wait_for(kNoWait) == std::future_status::ready) {
        PluginInstallResult r = installFuture_.get();
        installPending_ = false;
        installBusyName_.clear();
        // The point of the rescan: the file is on disk, but it is not a
        // PLUGIN until the host has loaded and validated it — and if it fails
        // validation the user needs to see that here, immediately, rather
        // than after a restart. It also re-runs enforcement, which is what
        // makes an update lift the retirement in the same frame it lands.
        //
        // Run on the ONE failure that still changed the disk too: applyUpdate
        // reports false with installedPath set when the new file installed but
        // the manifest could not be written.
        if (r.ok || !r.installedPath.empty()) { rescanPlugins(); }
        if (r.ok) {
            std::string reportBuf;
            cascade::core::formatUtf8(reportBuf,
                          r.isUpdate ? tr("Updated %s to %s") : tr("Installed %s to %s"),
                          r.name.c_str(), r.installedPath.c_str());
            installReport_ = reportBuf;
            if (!r.recordError.empty()) {
                // Honest partial state: verified bytes are installed, but the
                // plugin is unmanaged until a later install or catalogue fetch
                // repairs the record — and an unmanaged plugin is never
                // retired, which is the fail-open rule doing its job.
                std::string recordBuf;
                cascade::core::formatUtf8(recordBuf,
                              tr("%s was installed, but its record could not be written: %s. "
                                 "It will not be version-checked until that is repaired."),
                              r.name.c_str(), r.recordError.c_str());
                installError_ = recordBuf;
            }
        } else {
            installError_ = r.error;
        }
        // THE RUN KEEPS ITS OWN TALLY, here, where the single authority on
        // whether a transfer succeeded already is. Reading installReport_ back
        // out of the panel afterwards would be a second opinion about the same
        // event, and the two would eventually disagree.
        if (addAllRun_.active) {
            if (r.ok) {
                ++addAllRun_.installed;
            } else {
                ++addAllRun_.failed;
                addAllRun_.failures.emplace_back(r.name, r.error);
                cascade::core::diagLogf("plugin store: add all - %s FAILED: %s",
                                        r.name.c_str(), r.error.c_str());
            }
        }
    }
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

void Engine::removeInstalledPlugin(const std::string& fileName) {
    installError_.clear();
    installReport_.clear();

    // WINDOWS CANNOT DELETE A MAPPED IMAGE. A loaded plugin's DLL is open in
    // this process, and fs::remove on it fails with a sharing violation. Two
    // honest answers were available: unload first, or report the failure. We
    // unload — a Remove button that only works after a restart is not a
    // feature — and the rescan below reloads every survivor in the same
    // frame, so the visible effect is that ONE plugin disappears.
    //
    // Through detachAndUnloadPlugins(), NOT unloadAll() directly. This called
    // unloadAll() on its own, on the since-falsified premise that "nothing in
    // the product holds a decoder instance across frames yet" — the runner,
    // the panel/track-source UI handles, the basemap and the track-info
    // client all do, which is exactly why rescanPlugins() takes them off in a
    // prescribed order first. Unmapping a module out from under live handles
    // is undefined behaviour in third-party code, and whatever it does next it
    // is not "remove one plugin".
    //
    // If the delete still fails — the file is open in another process, or
    // permissions changed — PluginRepo's reason is shown verbatim in red and
    // the rescan puts everything back exactly as it was. Nothing is lost and
    // nothing is claimed that did not happen.
    detachAndUnloadPlugins();
    std::string err;
    if (pluginRepo_.remove(pluginDir_, fileName, err)) {
        std::string removedBuf;
        cascade::core::formatUtf8(removedBuf, tr("Removed %s"), fileName.c_str());
        installReport_ = removedBuf;
    } else {
        installError_ = err;
    }
    rescanPlugins();
}

void Engine::refreshPluginRunner() {
    // Decoder instances are created FOR a sample rate and a centre frequency,
    // so they are only valid for the source that was active when they were
    // built. Both change when the user switches source, opens a file, or when
    // a saved configuration restores a device at startup.
    //
    // That last case is what made this necessary. rescanPlugins() runs during
    // construction, BEFORE the config restores the source, so the first build
    // saw the generator's defaults - and an ADS-B plugin created against a
    // 100 MHz centre correctly reported "receiver is at 100.000 MHz" while the
    // radio sat on 1090 MHz. The decoder was right and the host had lied to it.
    //
    // Rebuilding rather than retuning, because the RATE cannot be changed on a
    // live instance: the ABI passes it to create() and nothing else.
    //
    // THE STOP LIST GOES DOWN FIRST, before either rebuild, because it decides
    // what those rebuilds create. Pushed on every refresh rather than only when
    // it changes: this function runs after every rescan, and a rescan is
    // exactly when the two halves have been cleared and could otherwise start a
    // plugin the user stopped.
    pluginRunner_.setStopped(pluginsStopped_);
    pluginUi_.setStopped(pluginsStopped_);
    // THE HOST TABLE FIRST, THEN THE DECODERS (0.99.31). The ABI promises that
    // attach() runs before any capability's create() (CascadeHostClientApi);
    // the runner used to be rebuilt first, so on the first rebuild of a
    // session a decoder's create() ran before its plugin had been handed the
    // table. With host API level 1 a plugin reads its settings and the
    // receiver from there, so the promise is now kept: the GUI half (which is
    // what attaches) is rebuilt here, and the runner below it.
    //
    // The GUI-side capabilities are rebuilt HERE too, and for the same reason:
    // a track source created against one receiver state should not survive a
    // source change. Doing it in this one function means the two halves of the
    // plugin system can never disagree about which plugins are live.
    //
    // Services are installed BEFORE rebuild, because the ABI promises attach()
    // runs before any capability's create() and a tracker may read the
    // receiver while building its initial state.
    cascade::core::HostServices svc;
    svc.centreHz = [this] { return pipeline_.activeSource().centerFrequencyHz(); };
    svc.sampleRateHz = [this] { return pipeline_.inputRateHz(); };
    svc.unixTimeMs = [] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    svc.tune = [this](double hz) -> std::int32_t {
        // PluginUi has already applied the per-plugin permission; this only
        // has to tune and report what the device said. There is no isOpen() on
        // the IqSource interface, and a source with no usable rate is one
        // nothing has opened - a state a plugin needs told apart from "the
        // device refused".
        if (!(pipeline_.activeSource().sampleRateHz() > 0.0)) {
            return CASCADE_TUNE_NO_DEVICE;
        }
        retuneSourceHz(hz);
        return CASCADE_TUNE_OK;
    };
    pluginUi_.setServices(std::move(svc));
    // THE GRANTS GO IN BEFORE THE REBUILD as well as after it (below): a
    // plugin may ask for the receiver from inside its attach(), and must be
    // answered by the grant the user gave, not by a clear() the rescan did.
    applyPluginTuneGrants();
    applyPluginSettingsGrants();
    pluginUi_.rebuild(pluginHost_.plugins());
    // The receiver's stream clock, which the runner writes and the
    // level-1 get_stream_info reads (CascadeStreamInfo).
    pluginRunner_.setStreamClock(pluginUi_.api().streamClock());
    pipeline_.setPluginRunner(nullptr);
    if (patchRunning_) {
        // THE PATCH HAS THE DECODERS (0.99.18). While it runs the
        // receiver runs only on the generator, and a plugin publishes its map
        // targets through ONE snapshot per module (the ADS-B plugin says so in
        // its own source) - so the receiver's ADS-B instance, fed generator
        // noise, would overwrite the aircraft the patch's ADS-B instance is
        // finding, every block. No receiver decoders while the patch runs;
        // stopping it calls this again and they come back.
        pluginRunner_.clear();
    } else {
        pluginRunner_.rebuild(pluginHost_.plugins(), cascade::core::Pipeline::kAudioRateHz,
                              pipeline_.inputRateHz(),
                              pipeline_.activeSource().centerFrequencyHz());
        pipeline_.setPluginRunner(&pluginRunner_);
    }
    // AND THE MUTE SNAPSHOT AFTER THE REBUILD, not before it. It carries the
    // running state, and running now means the runner is ACTUALLY FEEDING the
    // plugin (see rebuildMuteStates) - a question only the rebuild above can
    // answer, because it is the rebuild that decides which decoders the
    // receiver's current rate can drive. Built first, this would answer it
    // from the receiver the user has just left.
    rebuildMuteStates();

    // DEMONSTRATION instruments open their windows by themselves. This is the
    // one deliberate exception to "nothing opens but by your hand" (0.79.1),
    // and it is safe because a demonstration exists only when a developer set
    // FOXSDR_DEMO_INSTRUMENT for this launch: its whole purpose is to put a
    // face on screen to be looked at, and a switch that then required a
    // second step would be a switch for nothing.
    for (const cascade::core::HostInstrument& in : pluginUi_.instruments()) {
        if (in.plugin == "Demonstration") {
            host_.showDemonstrationInstrument(in);
        }
    }

    // THE BASEMAP, if a plugin supplies one. The first loaded plugin declaring
    // the capability wins: two basemaps cannot both be the map, and picking
    // silently by load order is more predictable than picking by some quality
    // the user cannot see. Detached first so a rescan that removed the plugin
    // takes its tiles - and its textures - with it.
    //
    // A STOPPED plugin is skipped here too, and that is the whole of what
    // "stopped" means for a basemap: the map falls back to its built-in
    // coastlines rather than keeping tiles from a plugin the user has switched
    // off. Skipping it in the runner and the UI half but not here would leave
    // the stopped plugin drawing the entire map background.
    const CascadeBasemapApi* wantBasemap = nullptr;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (lp.loaded && lp.basemap != nullptr &&
            !pluginIsStopped(cascade::core::pluginKey(lp))) {
            wantBasemap = lp.basemap;
            break;
        }
    }
    host_.attachBasemap(wantBasemap);
    // Track enrichment, by the same first-wins rule and for the same reason.
    const CascadeTrackInfoApi* wantTrackInfo = nullptr;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (lp.loaded && lp.trackInfo != nullptr &&
            !pluginIsStopped(cascade::core::pluginKey(lp))) {
            wantTrackInfo = lp.trackInfo;
            break;
        }
    }
    host_.attachTrackInfo(wantTrackInfo);
    // Grants LAST, and every time. rescanPlugins() calls PluginUi::clear(),
    // which drops the permission set along with the instances, so without this
    // a rescan would silently revoke every permission the user had given — and
    // the tracker that worked a moment ago would go quiet with no explanation.
    applyPluginTuneGrants();
    applyPluginSettingsGrants();
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

void Engine::applyPluginPreset(const cascade::core::LoadedPlugin& p,
                                  const CascadePreset& ps) {
    // A PRESET ON A STOPPED PLUGIN STARTS IT. Pressing "ADS-B 1090 MHz" is an
    // unambiguous "I want this plugin now", and the alternative is the worst
    // outcome this feature can produce: the radio dutifully retunes to 1090 MHz
    // in the mode the plugin asked for, and then nothing decodes, because the
    // plugin the user just pressed a button on is switched off. Only recorded
    // here — the rebuild at the end of this function is the one that creates
    // the instances, and it has to happen after the receiver has moved anyway.
    //
    // The row keeps its Stop button, so this is undone with one click.
    recordPluginStopped(cascade::core::pluginKey(p), false);

    // MODE FIRST, because the mode's default bandwidth would otherwise
    // overwrite the one the preset asked for.
    if (ps.demodMode != CASCADE_DEMOD_UNCHANGED && ps.demodMode <= CASCADE_DEMOD_RAW) {
        // The ABI's numbering is deliberately its own rather than an alias of
        // this table's order: a plugin compiled today must not change meaning
        // because the host reordered its buttons.
        static const int kAbiToIndex[9] = {
            -1,  // UNCHANGED, handled above
            0,   // NFM
            1,   // WFM
            2,   // AM
            3,   // DSB
            4,   // USB
            5,   // CW
            6,   // LSB
            7    // RAW
        };
        const int idx = kAbiToIndex[ps.demodMode];
        if (idx >= 0) {
            modeIndex_ = idx;
            pipeline_.setDemodMode(kModeMap[idx]);
            bandwidthIndex_ = kModeDefaultBw[idx];
            vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
        }
    }

    // The device rate, before the tune, because changing it re-plans the whole
    // chain. Advisory: a source that refuses simply keeps the rate it had, and
    // the decoder will say so itself rather than the host guessing.
    if (ps.sampleRateHz > 0.0 && device_ != nullptr &&
        pipeline_.activeSource().sampleRateHz() != ps.sampleRateHz) {
        const cascade::gui::RateSetOutcome set =
            cascade::gui::applySourceRate(*device_, ps.sampleRateHz, sourceError_);
        if (set.ok) {
            // A rate the driver COERCED is said (the refusal stays advisory,
            // as above).
            sourceError_ = set.sourceError;
            // The combo follows, or it would keep showing the rate the user
            // last picked while the radio ran at another.
            deviceRateIndex_ =
                nearestIndex(deviceRatesHz_, pipeline_.activeSource().sampleRateHz());
            followInputRate();
        }
    }

    if (ps.bandwidthHz > 0.0) {
        const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
        vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(ps.bandwidthHz, bwHi));
        pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
        // -1 WHEN THE PRESET ASKED FOR SOMETHING THE LIST DOES NOT CARRY, and
        // that is exactly the APT case: 40 kHz is not a step, and the nearest
        // one is 12.5 kHz. Pointing the combo there made it letter a bandwidth
        // the receiver was not running and, on the next click, apply it.
        bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
    }

    // WHERE the frequency goes differs by decoder kind, and getting it wrong
    // half-works in a way that is hard to diagnose. An I/Q decoder is handed
    // the whole raw device band and tunes inside it, so the BAND must contain
    // its signal - hence the device centre. An audio decoder wants its signal
    // in the tuned channel, so the VFO goes there and the offset is preserved.
    //
    // A DEVICE-CENTRE PRESET ALSO ZEROES THE VFO OFFSET, through the exact
    // setter path the VFO slider uses (so the slider follows). Without this a
    // remembered offset walked the device off the centre the decoder asked
    // for — see engine/tune_control.hpp's presetVfoOffsetHz for the measurement
    // this fixes (a -12 kHz offset put the ADS-B preset's counter at
    // 1089.988 MHz, not 1090.000 MHz). isPluginPreset=true on both branches:
    // the frequency this button asked for is the whole reason a mismatch
    // notice needs to say "this preset needs a receiver that covers that
    // band" rather than leaving it as an unexplained tune.
    if ((ps.flags & CASCADE_PRESET_DEVICE_CENTRE) != 0u) {
        const double off = cascade::gui::presetVfoOffsetHz(ps.flags, pipeline_.vfoOffsetHz());
        pipeline_.setVfoOffsetHz(off);
        vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
        retuneSourceHz(ps.frequencyHz, /*isPluginPreset=*/true);
    } else {
        tuneAbsoluteHz(ps.frequencyHz, /*isPluginPreset=*/true);
    }

    // Rebuild the decoders against the receiver they are now pointed at: the
    // rate and centre are passed to create() and cannot be changed on a live
    // instance, so without this the plugin the user just clicked would still
    // be configured for wherever the radio used to be.
    refreshPluginRunner();

    // THE PRESET BUTTON OPENS THE PLUGIN'S WINDOWS, EXPLICITLY - AND IT USED
    // TO OPEN ONLY ONE KIND OF THEM. Pressing it is the one unambiguous "show
    // me this plugin" gesture the product offers, its tooltip has always said
    // so, and since 0.79.1 a window opens by such a gesture and by nothing
    // else. A MapPage was all this opened, so pressing the NOAA APT preset
    // retuned the radio, started the decoder and put nothing on screen: the
    // picture window the whole plugin exists to fill stayed shut, with its row
    // further down a rail the user had no reason to scroll.
    //
    // DECIDED FROM WHAT THE MODULE DECLARES, not from what it has published
    // yet. A decoder that has produced no picture still gets its window, which
    // says it is waiting - the truth, and what was asked to be seen. Each id
    // is the one drawPluginWindows draws by, built from the same display name.
    host_.openPluginWindowsFor(p);

    std::string note;
    cascade::core::formatUtf8(note, tr("Tuned to %.4f MHz for %s"), ps.frequencyHz / 1.0e6,
                  p.name.c_str());
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

void Engine::applyPluginApi() {
    cascade::core::PluginApiCore& api = pluginUi_.api();

    // --- 1. What plugins asked of the receiver -------------------------------
    //
    // Applied in the order they were asked, each as the SAME command the
    // desktop control of that kind sends (net::pluginControlToCommand), through
    // applyCommand - so a plugin can do nothing to the receiver that the user
    // could not do themselves, and gets every clamp and readback the user
    // would. The permission is checked AGAIN here: a grant revoked, or a
    // plugin stopped, between the request and this frame must stop the
    // request from acting (controlStillAllowed).
    std::vector<cascade::core::PluginControl> controls;
    api.takeControls(controls);
    for (const cascade::core::PluginControl& c : controls) {
        if (!api.controlStillAllowed(c)) { continue; }
        FoxCommand command{};
        if (cascade::net::pluginControlToCommand(c, command)) { (void)applyCommand(command); }
    }

    // --- 2. What plugins said ---------------------------------------------------
    //
    // Into the decoder output, which is where a user already looks when a
    // plugin is quiet, one line per line of text. WARN and ERROR also become
    // the notice on the plugin's plate in Fitted modules. NOT into the
    // diagnostic log: that file can travel with a problem report, and a
    // plugin's words are shown to the user, not collected (plugin_abi.h, log).
    std::vector<cascade::core::PluginLogLine> lines;
    api.takeLog(lines);
    for (const cascade::core::PluginLogLine& l : lines) {
        std::size_t from = 0;
        while (from <= l.text.size()) {
            std::size_t to = l.text.find('\n', from);
            if (to == std::string::npos) { to = l.text.size(); }
            std::string part = l.text.substr(from, to - from);
            if (!part.empty() && part.back() == '\r') { part.pop_back(); }
            if (!part.empty()) {
                cascade::core::DecodedLine d;
                d.plugin = l.name;
                d.text = std::move(part);
                decoderLog_.push_back(std::move(d));
            }
            from = to + 1;
        }
        if (l.level >= CASCADE_LOG_WARN) { pluginNotices_[l.key] = PluginNotice{l.level, l.text}; }
    }

    // --- 3. What plugins stored ------------------------------------------------
    //
    // Copied into the durable map only when the store actually changed, so an
    // idle frame costs one lock and one compare. currentConfig() saves the
    // durable map through the ordinary debounced, off-thread config write.
    const std::uint64_t gen = api.settingsGeneration();
    if (gen != pluginSettingsGen_) {
        pluginSettingsGen_ = gen;
        pluginSettings_ = api.settingsSnapshot();
    }

    // --- 4. What plugins marked ------------------------------------------------
    const std::uint64_t ms = api.markersSeq();
    if (ms != pluginMarkersSeq_) {
        pluginMarkersSeq_ = ms;
        api.markers(pluginMarkers_);
    }

    // (What plugins may READ is the one receiver snapshot, published by
    // publishReceiverState later in this frame - after the scanner and before
    // anything is drawn - so a control applied above is already in the
    // snapshot a plugin reads this frame, exactly as when this function
    // published a snapshot of its own.)
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

void Engine::setPluginStopped(const std::string& pluginKey, bool stopped) {
    // THE SAME LIFECYCLE PATH AS EVERYTHING ELSE, deliberately. Stopping could
    // have destroyed one plugin's instances in place, and that is precisely
    // the second lifecycle this avoids: the retune grant, the basemap, the
    // track-info client and the panel windows are all wired up in
    // refreshPluginRunner, so a bespoke teardown would have to repeat every one
    // of them and would drift from the original the first time one changed.
    // Rebuilding costs the other plugins one create()/destroy() pair on a user
    // action that happens seconds apart at worst.
    recordPluginStopped(pluginKey, stopped);
    refreshPluginRunner();

    // ONLY ON A START, and only through THIS path. This is the "Start" key on
    // the fitted-modules row (drawFittedModulesWindow's FittedModulesAction::
    // Kind::Start), which is presently the sole place a plugin transitions
    // from stopped to running — a preset BUTTON also starts a stopped plugin
    // (applyPluginPreset's own recordPluginStopped(..., false)) but calls
    // recordPluginStopped directly rather than through here, precisely so an
    // explicit preset press is never second-guessed by this. See
    // maybeAutoPresetOnStart's own comment for what "already inside" means.
    if (!stopped) { maybeAutoPresetOnStart(pluginKey); }
}

void Engine::maybeAutoPresetOnStart(const std::string& pluginKey) {
    maybeAutoPreset(pluginKey, "started");
}

void Engine::maybeAutoPresetOnShow(const std::string& pluginKey) {
    maybeAutoPreset(pluginKey, "window opened");
}

void Engine::maybeAutoPreset(const std::string& pluginKey, const char* verb) {
    // FIND THE PLUGIN THIS KEY NAMES. Both callers only carry a file name —
    // the same identity recordPluginStopped and the tune grant use — never a
    // LoadedPlugin, so the object with its preset table has to be looked back
    // up here, the same way the web control's remote preset apply does it
    // (this file's r.pluginPresetIndex handling).
    const cascade::core::LoadedPlugin* found = nullptr;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (cascade::core::pluginKey(p) == pluginKey) {
            found = &p;
            break;
        }
    }
    if (found == nullptr) { return; }

    // THE SAME ENUMERATION drawPluginPresets uses, so this can never see a
    // different set of presets than the row the user could have pressed
    // instead: bounded, and each frequency positively tested so third-party
    // garbage (NaN included) never reaches the decision below.
    std::vector<CascadePreset> own;
    for (const cascade::gui::IndexedPreset& ip : validatedPresets(*found)) {
        own.push_back(ip.preset);
    }
    // THE USER'S OWN PRESETS COME FIRST (0.99.4). autoPresetIndexOnStart
    // applies element 0 unless the receiver already sits on any element, so a
    // channel the user saved against this plugin is what opening it tunes to
    // - and being on it, or on any of the plugin's own, tunes nowhere. Before
    // this, a UK listener on 153.050 MHz was moved to POCSAG's DAPNET preset
    // every time they opened POCSAG. See engine/tune_control.hpp.
    const std::vector<CascadePreset> presets =
        cascade::gui::autoPresetCandidates(userPresetsForPlugin(*found), own);
    if (presets.empty()) { return; }

    // WHAT THE RECEIVER IS DOING RIGHT NOW — the same three numbers
    // applyPluginPreset itself moves, read back rather than assumed, so
    // "already inside" means the same thing here as it does to the button.
    const double deviceCentreHz = pipeline_.activeSource().centerFrequencyHz();
    const double vfoOffsetHz = pipeline_.vfoOffsetHz();
    const double deviceRateHz = pipeline_.activeSource().sampleRateHz();

    const int idx = cascade::gui::autoPresetIndexOnStart(presets, deviceCentreHz, vfoOffsetHz,
                                                          deviceRateHz);
    if (idx < 0) { return; }

    const CascadePreset& ps = presets[static_cast<std::size_t>(idx)];
    // THE IDENTICAL PATH THE BUTTON TAKES: mode, bandwidth, device rate, the
    // tune itself and the plugin's own windows. Starting a decoder (or
    // opening its window) is meant to feel like pressing its preset for it,
    // not a cut-down copy of doing so.
    applyPluginPreset(*found, ps);

    char label[CASCADE_PRESET_LABEL_CHARS + 1];
    std::snprintf(label, sizeof(label), "%.*s", CASCADE_PRESET_LABEL_CHARS,
                  ps.label[0] != '\0' ? ps.label : found->name.c_str());
    cascade::core::diagLogf("plugin: %s %s - applied its preset %s", found->name.c_str(), verb,
                            label);
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

void Engine::stopMutingPlugins(const std::vector<std::string>& keys) {
    // EXACTLY THE PLUGINS THE MESSAGE NAMED, which is why the keys are an
    // ARGUMENT and not "every running plugin that mutes". Measured on the
    // running application: the wider rule also stopped AIS, which was running
    // 900 MHz away from its own preset and therefore muting nothing, from a
    // dialog whose sentence said "ADS-B". A button must do what the words above
    // it say - so the popup passes what it captured when it opened, and the
    // banner passes what it is displaying.
    //
    // COPIED FIRST because every caller's vector is one of the members cleared
    // below; iterating the caller's own storage while emptying it would be a
    // use-after-clear on the second plugin.
    //
    // All of them in one pass, with a SINGLE rebuild at the end: stopping one
    // at a time would rebuild every other plugin's instances once per plugin
    // stopped, and would leave the audio still muted after the first of what
    // the user read as one decision.
    const std::vector<std::string> copy = keys;
    for (const std::string& k : copy) { recordPluginStopped(k, true); }
    muteKeptRunning_ = false;
    mutedBy_.clear();
    mutedByKeys_.clear();
    mutePopup_ = cascade::core::MutePopupSubject{};
    mutePopupQueued_ = false;
    mutePrevOnPreset_ = false;
    refreshPluginRunner();
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

void Engine::removeBlockedPlugin(const std::string& fileName) {
    installError_.clear();
    installReport_.clear();

    // THROUGH detachAndUnloadPlugins(), NEVER unloadAll() DIRECTLY. This used
    // to call unloadAll() on its own, on the reasoning that a blocked plugin
    // was never loaded so there was nothing to detach - which is true of the
    // blocked plugin and false of every OTHER plugin, all of which unloadAll()
    // unmaps too, with their track-source, panel and decoder instances still
    // alive. A live handle is memory inside a module that has just been
    // unmapped, and a plugin whose static state owns a thread (Satellites
    // 1.0.1) reached std::terminate in its own CRT the moment that happened:
    // an abort() the crash handler never sees. The ordered sequence exists so
    // there is exactly one way to take a module down; this was the one caller
    // that did not use it.
    detachAndUnloadPlugins();
    std::string err;
    if (pluginRepo_.removeQuarantined(pluginDir_, fileName, pluginQuarantineSuffix(), err)) {
        // The same sentence removeInstalledPlugin() writes, and translated the
        // same way: it was the one install/remove report still joined in
        // English. Built as a string, since a file name has no set length.
        const char* fmt = tr("Removed %s");
        std::vector<char> buf(std::strlen(fmt) + fileName.size() + 8);
        cascade::core::formatUtf8(buf.data(), buf.size(), fmt, fileName.c_str());
        installReport_ = buf.data();
    } else {
        installError_ = err;
    }
    rescanPlugins();
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

void Engine::startReceiver() {
    // THE ONE WAY THE RECEIVER IS STARTED, and it exists for the fault edge.
    // endTakesOnFault acts once per fault, at the top of each frame; a start
    // clears the pipeline's latch. So a fault that lands after this frame's
    // check and is then cleared by a start in the same frame would never be
    // seen at all - and the take would carry on across it. Asked here, while
    // the latch is still up.
    endTakesOnFault();
    pipeline_.start();
    // A start from a faulted, stopped pipeline clears the latch, so whatever
    // the latch does next is a NEW fault: forget the old edge. Without this a
    // second fault landing within a frame of the START (a file or radio that
    // is still broken) found faultSeen_ still true and was never acted on -
    // a take armed after the first fault ran straight on through it. A start
    // that found the pipeline already running changed nothing, and then the
    // latch is down and faultSeen_ already false.
    faultSeen_ = false;
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

void Engine::openIqFile(const std::string& path) {
    // The Source panel's IQ file Open. The pipeline keeps its current source
    // until the open succeeds: a failed open constructs and destroys a
    // throwaway IqFileSource without ever touching the pipeline.
    auto file = std::make_unique<cascade::source::IqFileSource>();
    if (!file->open(path)) {
        sourceError_ = file->lastError();
        return;
    }
    // Carry the displayed frequency over: a file's center is nominal anyway,
    // and a readout that jumps to 0 on source switch would read as a tuning
    // bug. The AIR frequency carries over; the file is told it through its
    // own converter, which is off unless the user set one for I/Q files.
    const double fileRadioHz =
        radioHzForSource("file", std::string(), pipeline_.activeSource().centerFrequencyHz());
    if (fileRadioHz >= 0.0) { file->setCenterFrequencyHz(fileRadioHz); }
    device_ = nullptr;  // before setSource destroys a live device
    soapyView_ = nullptr;
    deviceArgs_.clear();
    deviceModel_.clear();
    sourceError_.clear();
    ++sourceGen_;  // a device open still in flight is now stale
    installSource(std::move(file));
    sourceKind_ = "file";
    applyConverterForSource();
    iqOpenPath_ = path;
    // A file is a deliberate choice of source like any other, so a radio
    // remembered from a failed restore is superseded here too - see
    // selectSource's generator row.
    restoreKeep_ = cascade::gui::RememberedSource{};
    restoreKeepLabel_.clear();
    followInputRate();  // DSP chain + frequency axis track the file's rate
    // The RATE, never the path: a file name is the user's own data and a
    // report is a support artefact, not a listening record.
    cascade::core::diagLogf("source: opened an I/Q file at %.0f S/s",
                            pipeline_.activeSource().sampleRateHz());
}

void Engine::tuneToBookmark(const cascade::core::Bookmark& b) {
    // Click-to-tune on a bookmark row, and the web remote's bookmarkTune -
    // the DESKTOP's order, which is the one both take since stage 1:
    // frequency through the shared absolute-tune path (same as scanner
    // retunes), then mode and bandwidth. An unknown mode name - a newer
    // build's file, kept verbatim by FreqManager on purpose - leaves the
    // current mode untouched.
    tuneAbsoluteHz(b.freqHz);
    for (int m = 0; m < 8; ++m) {
        if (b.mode == kModeNames[m]) {
            modeIndex_ = m;
            pipeline_.setDemodMode(kModeMap[m]);
            break;
        }
    }
    // Same clamp as the config restore: [3 kHz, 90% of channel rate].
    const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
    vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(b.bandwidthHz, bwHi));
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    // -1 for a bookmark saved at a bandwidth the list does not carry (one
    // taken while a preset had the VFO at 40 kHz, say): the combo letters the
    // real figure and ticks nothing.
    bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
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

bool Engine::selectSourceById(const std::string& id) {
    // THE IDS (docs/engine-stage1.md): "siggen"; "<kind>:<args>" for a scanned
    // device (kind "soapy" or a native driver key); "row:iqfile" and
    // "row:soundcard" for the two rows that only show their panel;
    // "file:<path>" and "open-pluto:<args>" for the panels' Open keys; and
    // "soundcard:open" for the sound card panel's Open, which opens the card
    // the panel describes (its settings are not in the id - OPEN, see the
    // doc). Every scanned-device id is MATCHED against the enumerated lists,
    // never handed to a driver verbatim: a browser must not be able to pass
    // arbitrary kwargs to a vendor module.
    if (id == "siggen") {
        selectSource(0);
        return true;
    }
    if (id == "row:iqfile") {
        selectSource(1);
        return true;
    }
    if (id == "row:soundcard") {
        selectSource(kSoundCardRow);
        return true;
    }
    if (id == "soundcard:open") {
        sourceError_.clear();
        launchSoundCardOpen(false, soundCard_);
        return true;
    }
    const std::size_t colon = id.find(':');
    if (colon == std::string::npos) { return false; }
    const std::string kind = id.substr(0, colon);
    const std::string args = id.substr(colon + 1);
    if (kind == "file") {
        openIqFile(args);
        return true;
    }
    if (kind == "open-pluto") {
        openPlutoAt(args);
        return true;
    }
    // The KIND is matched too - a native row and a Soapy row for one dongle
    // carry the same serial.
    if (cascade::gui::isNativeSourceKind(kind)) {
        for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
            if (nativeDevices_[i].driver == kind && nativeDevices_[i].args == args) {
                selectSource(kNativeRowBase + static_cast<int>(i));
                return true;
            }
        }
    } else {
        for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
            if (soapyDevices_[i].args == args) {
                selectSource(soapyRowBase() + static_cast<int>(i));
                return true;
            }
        }
    }
    sourceError_ = "no scanned device matches those arguments; rescan and try again";
    return false;
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

void Engine::tuneAbsoluteHz(double absHz, bool isPluginPreset) {
    // The same setter + readback path the toolbar digit wheel uses, with the
    // VFO offset preserved: command the SOURCE center so the VFO band lands
    // on absHz. A refusal (a tune the driver rejects) needs no handling —
    // every display, and the scanner's user-tune baseline, follows the
    // readback, which simply won't move. isPluginPreset is only ever true from
    // applyPluginPreset, forwarded so a mismatch this produces can say which
    // plugin's button asked for it — see applyRetuneNow.
    retuneSourceHz(absHz - pipeline_.vfoOffsetHz(), isPluginPreset);
}

void Engine::retuneSourceHz(double centerHz, bool isPluginPreset) {
    // Hardware tunes are PACED (see the header declaration): a burst becomes
    // one device call per interval with the latest value. Everything without
    // a USB control path underneath applies immediately.
    //
    // The SCANNER also bypasses the pacing: tickScanner() reads the frequency
    // back synchronously after every commanded tune to set its user-wins
    // baseline, and a tune held by the coalescer would make that readback
    // stale — the scanner would then see its own deferred retune land a frame
    // later and stop itself, misreading it as the user's hand. The scanner is
    // a control loop paced by its own dwell, not a human gesture burst.
    //
    // pendingRetuneIsPreset_ carries isPluginPreset across the coalescer the
    // same way retuneCoalescer_ itself carries centerHz: overwritten on every
    // call, so whichever value was requested LAST is the one a deferred
    // apply sees — "latest wins" for the context, not just the frequency.
    pendingRetuneIsPreset_ = isPluginPreset;
    // A SOURCE WITH NO TUNER (a sound card) is tuned by its VFO instead - see
    // gui::tuneWithFixedCentre.
    if (retuneFixedCentre(centerHz)) { return; }
    if (device_ == nullptr || scanner_.active()) {
        applyRetuneNow(centerHz, isPluginPreset);
        return;
    }
    if (retuneCoalescer_.request(centerHz, steadyNowMs())) {
        applyRetuneNow(centerHz, isPluginPreset);
    }
}

void Engine::pollPendingRetune() {
    if (const std::optional<double> hz = retuneCoalescer_.due(steadyNowMs())) {
        applyRetuneNow(*hz, pendingRetuneIsPreset_);
    }
}

void Engine::applyRetuneNow(double centerHz, bool isPluginPreset) {
    // ONE place where the source centre moves. The pipeline cannot observe a
    // device retune (the source owns the tuner), so the RDS/stereo decoders
    // have to be told explicitly — otherwise the previous station's PS name
    // stays on screen over the new one, which is a wrong readout, not a
    // cosmetic lag. No-op when the tune does not actually move anything, so
    // a repeated command cannot keep the decoders permanently reset.
    cascade::source::IqSource& src = pipeline_.activeSource();
    if (src.centerFrequencyHz() == centerHz) { return; }
    // What the rate and the source's own error line were BEFORE the tune: a
    // tune can move the rate too (below).
    const double rateBeforeHz = src.sampleRateHz();
    const std::string errBefore = src.lastError();
    const bool applied = src.setCenterFrequencyHz(centerHz);
    // Out-of-band applies (a device open's carry-across) pace the next burst
    // off this moment too, so the coalescer's clock never lags an apply.
    retuneCoalescer_.noteApplied(steadyNowMs());
    pipeline_.resetRds();
    // I/Q decoders are told for the same reason: they work on the raw band and
    // several of them (ADS-B, AIS) report when the receiver is nowhere near
    // the frequency they need. Reading back from the source rather than
    // trusting the requested value, because a device may land on a nearby
    // tuning step and the decoder should be told where it actually is.
    const double landedHz = src.centerFrequencyHz();
    pluginRunner_.retune(landedHz);

    // SAY WHEN THE RADIO CANNOT TUNE THERE. Measured on a USRP B200: a request
    // for 7.000 MHz landed at 30.800 MHz with sourceError_, faultMessage and
    // the log all silent — the counter simply showed the wrong figure and the
    // plugin it was for looked broken instead of the hardware. The comparison
    // and the wording live in engine/tune_control.hpp so they are testable
    // without an open device; this call site only supplies what the device
    // actually said.
    //
    // ONLY A TUNE THAT WAS APPLIED CAN HAVE BEEN COERCED. A source that
    // refused the call - the driver busy behind its control lock during a
    // fast VFO drag, a vendor fault, an exception - leaves its readback where
    // it was and says so through sourceError; comparing that stale readback
    // with the request logged "asked for 124.19 MHz, the B200 answered
    // 124.69 MHz", a coercion that never happened (seen on the desk the day
    // this check was added).
    if (applied) {
        noteTuneMismatch(centerHz, landedHz, isPluginPreset);
    } else {
        noteTuneRefused(centerHz, isPluginPreset);
    }

    // A TUNE THAT MOVED THE RATE. An RX888 tuned from HF into VHF at more than
    // 8 MS/s narrows its rate to what the tuner's IF allows, inside
    // setCenterFrequencyHz, and says so through lastError() on a call that
    // returned true. Through 0.99.34 nothing here looked: the chain kept
    // running at the old rate (a wrong span, every decoder on the wrong time
    // base), the Rate combo kept the old entry, and the reason was unread.
    if (applied) {
        const cascade::gui::RetuneRateOutcome moved =
            cascade::gui::rateAfterRetune(src, rateBeforeHz, errBefore, sourceError_);
        if (moved.rateMoved) {
            sourceError_ = moved.sourceError;
            if (device_ != nullptr) {
                deviceRateIndex_ = nearestIndex(deviceRatesHz_, moved.rateHz);
            }
            followInputRate();
        }
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

void Engine::scannerFrame() {
    if (!scanner_.active()) { return; }

    // User wins: any tune this frame that the scanner did not command —
    // digit wheel, VFO drag/slider (the offset is part of the absolute
    // frequency), bookmark click, source switch — leaves the readback off
    // the last commanded value, and the scan stops rather than fight the
    // user's hands. Checked BEFORE tick so the stale squelch state of a
    // just-abandoned frequency can never emit one more retune.
    if (scannerHasExpected_ &&
        std::fabs(currentAbsoluteHz() - scannerExpectedAbsHz_) > kScanUserTuneEpsHz) {
        scanner_.stop();
        return;
    }

    // Squelch-open per the squelch's own OPEN comparison (Squelch::process
    // opens at channel power > threshold; > , not >=): same power quantity,
    // same threshold value (squelchDb_ mirrors what setSquelchDb pushed).
    // Documented approximation: the reading comes from the pipeline's
    // S-meter snapshot — an EMA ~100x slower than the squelch's internal
    // meter — and the close-side hysteresis/hold are not replicated. For
    // the scan decision only "is a signal present now" matters, and the
    // S-meter is the one channel-power readout that is lock-free from the
    // GUI thread.
    const bool squelchOpen = pipeline_.signalPowerDb() > squelchDb_;
    const std::optional<double> retune =
        scanner_.tick(host_.frameTimeS() * 1000.0, squelchOpen);
    if (retune.has_value()) {
        tuneAbsoluteHz(*retune);
        // Baseline from READBACK, not the request: a device that coerces
        // the tune must not read as a user action next frame.
        scannerExpectedAbsHz_ = currentAbsoluteHz();
        scannerHasExpected_ = true;
    }
}

double Engine::publishReceiverState() {
    // THE ONE PUBLISH (engine stage 2). Two reads are made once and handed to
    // both halves: the fault sentence (a string copy under the pipeline's
    // lock) and the RDS snapshot (likewise).
    const std::string faultMessage = pipeline_.faultMessage();
    const cascade::core::RdsSnapshot rds = pipeline_.rdsSnapshot();
    cascade::core::PublishedState ps{};
    fillPublishedState(ps, faultMessage, rds);
    std::shared_ptr<cascade::net::RadioStatus> lists = std::make_shared<cascade::net::RadioStatus>();
    std::vector<std::uint64_t> bookmarkIds;
    fillStatusLists(*lists, faultMessage, rds, bookmarkIds);
    receiverSnapshot_->publish(ps, std::move(lists), std::move(bookmarkIds));
    // The spectrum frame for the browser is the window's (a stream, not
    // state: AppWindow::publishWebSpectrum), labelled with the centre just
    // published.
    return ps.rx.centreHz;
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

void Engine::submitCommand(const FoxCommand& c) {
    cascade::core::cmd::QueuedCommand q;
    q.c = c;
    submitCommand(std::move(q));
}

void Engine::submitCommand(cascade::core::cmd::QueuedCommand q) {
    LocalCommand lc;
    lc.q = std::move(q);
    lc.sourceGen = sourceGen_;
    localCommands_.push_back(std::move(lc));
}

void Engine::drainLocalCommands() {
    if (localCommands_.empty()) { return; }
    // TAKEN, THEN APPLIED: a command's own effects may queue more (none does
    // today); those wait for the next drain rather than extend this one.
    std::vector<LocalCommand> batch;
    batch.swap(localCommands_);
    for (const LocalCommand& lc : batch) {
        // A radio command asked of a radio that has gone since (closed, or
        // replaced by one that opened in between - sourceGen_ moves with
        // every device_ change, including one made earlier in this batch) is
        // dropped: it was drawn for that radio, never for this one.
        if (lc.sourceGen != sourceGen_ && isDeviceScoped(lc.q.c.op)) { continue; }
        (void)applyCommand(lc.q.c, lc.q.longText);
    }
}

FoxCommandResult Engine::applyCommand(const FoxCommand& c, const std::string& longText) {
    namespace cmd = cascade::core::cmd;
    FoxCommandResult res = commandResultFor(c);
    // A refusal changes NOTHING and says why (the API's rule 2).
    const auto refuse = [&res](std::int32_t status, const char* why) {
        res.status = status;
        res.flags |= FOXAPI_RESULT_REFUSED;
        std::snprintf(res.message, sizeof(res.message), "%s", why);
        return res;
    };
    // NaN and infinity never reach a setter from any client: every numeric
    // slot an op does not use is zero, so all four are checked. Ranges stay
    // where they were enforced before stage 1 (the web and CAT parsers, the
    // plugin host API, the widgets' own spans) - see docs/engine-stage1.md.
    for (const double v : c.num) {
        if (!std::isfinite(v)) { return refuse(FOXAPI_BAD_ARGUMENT, "not a finite number"); }
    }
    const bool on = c.ival[0] != 0;
    const std::string text = cmd::textOf(c, longText);

    switch (c.op) {
        // --- the receiver ----------------------------------------------------
        case FOXAPI_OP_RUN:
            // The dome, the Start/Stop key, the scope's POWER, the web remote
            // and a plugin all stop through stopReceiver - recordings first
            // (0.99.36) - and start through startReceiver.
            if (on) {
                startReceiver();
            } else {
                stopReceiver();
            }
            res.applied[0] = pipeline_.running() ? 1.0 : 0.0;
            return res;
        case FOXAPI_OP_SET_CENTRE:
            // The GUI's own absolute-tune path, so the RDS/stereo decoders are
            // told to forget the old station.
            retuneSourceHz(c.num[0]);
            res.applied[0] = pipeline_.activeSource().centerFrequencyHz();
            return res;
        case FOXAPI_OP_SET_FREQUENCY:
            // The counter's rule: the VFO offset kept, the centre follows.
            tuneAbsoluteHz(c.num[0]);
            res.applied[0] = currentAbsoluteHz();
            return res;
        case FOXAPI_OP_SET_VFO_OFFSET: {
            // Clamped against the LIVE rate: the whole band stays inside the
            // baseband (the web remote, a plugin and a spectrum drag).
            const double off = cascade::gui::vfoOffsetInsideSpan(c.num[0], pipeline_.inputRateHz(),
                                                                 vfoBandwidthHz_);
            pipeline_.setVfoOffsetHz(off);
            vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
            if (off != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            res.applied[0] = off;
            return res;
        }
        case FOXAPP_OP_SET_VFO_OFFSET_FREE:
            // The rail's VFO slider: its own +/-500 kHz span, never clamped to
            // the band (a divergence from SET_VFO_OFFSET kept as it was - see
            // docs/engine-stage1.md).
            vfoOffsetKhz_ = static_cast<float>(c.num[0] / 1000.0);
            pipeline_.setVfoOffsetHz(1000.0 * static_cast<double>(vfoOffsetKhz_));
            res.applied[0] = pipeline_.vfoOffsetHz();
            return res;
        case FOXAPP_OP_VFO_TO_ABSOLUTE:
            // Click-to-tune on the spectrum or the waterfall: the snap is to
            // the mode in force WHEN APPLIED, the offset against the centre
            // in force when applied.
            setVfoToAbsoluteHz(c.num[0], on);
            res.applied[0] = pipeline_.vfoOffsetHz();
            return res;
        case FOXAPI_OP_STEP_TUNE: {
            // The counter's wheel and switches, TUNE UP/DN and the tuning
            // keys: from where the receiver IS when this is applied, floored
            // at 0 Hz and at the counter's own floor (core::minTunedAirHz).
            if (c.ival[0] == 0) { return refuse(FOXAPI_OUT_OF_RANGE, "a step of zero steps"); }
            const double hz = std::max(0.0, currentAbsoluteHz());
            const double minTunedHz =
                cascade::core::minTunedAirHz(pipeline_.converter(), pipeline_.vfoOffsetHz());
            const double next = std::max(
                minTunedHz, std::max(0.0, hz + static_cast<double>(c.ival[0]) * c.num[0]));
            tuneAbsoluteHz(next);
            res.applied[0] = currentAbsoluteHz();
            return res;
        }
        case FOXAPI_OP_SET_MODE: {
            // FOXAPI_DEMOD_* is the mode keys' order, 1-based. The keys'
            // setModeIndex, so every client moves the bandwidth to the mode's
            // default and logs the change the same way.
            if (c.ival[0] < 1 || c.ival[0] > 8) { return refuse(FOXAPI_OUT_OF_RANGE, "no such mode"); }
            setModeIndex(static_cast<int>(c.ival[0] - 1));
            res.applied[0] = static_cast<double>(modeIndex_ + 1);
            res.applied[1] = vfoBandwidthHz_;
            return res;
        }
        case FOXAPI_OP_SET_BANDWIDTH: {
            // [3 kHz, 90% of the channel rate]; -1 for a width the steps do
            // not carry, so the combo letters the real figure and ticks none.
            const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
            vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(c.num[0], bwHi));
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
            if (vfoBandwidthHz_ != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            res.applied[0] = vfoBandwidthHz_;
            return res;
        }
        case FOXAPP_OP_SET_BANDWIDTH_STEP:
            // The rail's combo: one of the steps, exactly, never clamped.
            if (c.ival[0] < 0 || c.ival[0] >= kBwCount) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such bandwidth step");
            }
            bandwidthIndex_ = static_cast<int>(c.ival[0]);
            vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            res.applied[0] = vfoBandwidthHz_;
            return res;
        case FOXAPP_OP_SET_BANDWIDTH_DRAG: {
            // A band edge dragged on the spectrum: clamped like SET_BANDWIDTH,
            // but the combo's step is left where it was (as it always was).
            const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
            vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(c.num[0], bwHi));
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            if (vfoBandwidthHz_ != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            res.applied[0] = vfoBandwidthHz_;
            return res;
        }
        case FOXAPI_OP_SET_SQUELCH:
            squelchDb_ = static_cast<float>(c.num[0]);
            pipeline_.setSquelchDb(squelchDb_);
            res.applied[0] = squelchDb_;
            return res;
        case FOXAPI_OP_SET_VOLUME:
            volume_ = static_cast<float>(c.num[0]);
            pipeline_.audio().setVolume(volume_);
            res.applied[0] = volume_;
            return res;
        case FOXAPI_OP_SET_MUTED:
            // The user's own mute (the MUTE key's flag); updateAudioMute
            // applies it with the rest of the mute policy.
            userMuted_ = on;
            res.applied[0] = userMuted_ ? 1.0 : 0.0;
            return res;
        case FOXAPI_OP_SET_DEEMPHASIS:
            if (c.ival[0] < 0 || c.ival[0] >= kDeemphCount) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such de-emphasis");
            }
            deemphIndex_ = static_cast<int>(c.ival[0]);
            pipeline_.setDeemphasisUs(kDeemphUs[deemphIndex_]);
            res.applied[0] = deemphIndex_;
            return res;
        case FOXAPI_OP_SET_STEREO:
            stereoEnabled_ = on;
            pipeline_.setStereoEnabled(stereoEnabled_);
            return res;
        case FOXAPI_OP_SET_NR:
            nrEnabled_ = on;
            pipeline_.setNoiseReductionEnabled(nrEnabled_);
            if (c.ival[1] == 1) {
                nrStrength_ = static_cast<float>(c.num[0]);
                pipeline_.setNoiseReductionStrength(nrStrength_);
            }
            res.applied[0] = nrStrength_;
            return res;
        case FOXAPP_OP_SET_NR_STRENGTH:
            nrStrength_ = static_cast<float>(c.num[0]);
            pipeline_.setNoiseReductionStrength(nrStrength_);
            res.applied[0] = nrStrength_;
            return res;
        case FOXAPI_OP_SET_NOTCH:
            notchEnabled_ = on;
            pipeline_.setNotchEnabled(notchEnabled_);
            if (c.ival[1] == 1) {
                notchFreqHz_ = static_cast<float>(c.num[0]);
                pipeline_.setNotchFrequencyHz(static_cast<double>(notchFreqHz_));
                notchQ_ = static_cast<float>(c.num[1]);
                pipeline_.setNotchQ(static_cast<double>(notchQ_));
            }
            res.applied[0] = notchFreqHz_;
            res.applied[1] = notchQ_;
            return res;
        case FOXAPP_OP_SET_NOTCH_FREQUENCY:
            notchFreqHz_ = static_cast<float>(c.num[0]);
            pipeline_.setNotchFrequencyHz(static_cast<double>(notchFreqHz_));
            res.applied[0] = notchFreqHz_;
            return res;
        case FOXAPP_OP_SET_NOTCH_Q:
            notchQ_ = static_cast<float>(c.num[0]);
            pipeline_.setNotchQ(static_cast<double>(notchQ_));
            res.applied[0] = notchQ_;
            return res;
        case FOXAPI_OP_SET_AUTO_NOTCH:
            autoNotch_ = on;
            pipeline_.setAutoNotchEnabled(autoNotch_);
            return res;

        // --- the display -------------------------------------------------------
        // The minimum span is kept by pushing back the end that MOVED, so the
        // other one does not shift under the user; with both given, dbMin
        // yields. A degenerate or inverted span is a divide-by-zero where dB
        // is mapped to pixels.
        case FOXAPI_OP_SET_DISPLAY_RANGE:
        case FOXAPP_OP_SET_DISPLAY_MIN:
        case FOXAPP_OP_SET_DISPLAY_MAX: {
            const bool maxOnly = c.op == FOXAPP_OP_SET_DISPLAY_MAX;
            float lo = (c.op == FOXAPP_OP_SET_DISPLAY_MAX) ? dbMin_ : static_cast<float>(c.num[0]);
            float hi = (c.op == FOXAPI_OP_SET_DISPLAY_RANGE) ? static_cast<float>(c.num[1])
                       : maxOnly                              ? static_cast<float>(c.num[0])
                                                              : dbMax_;
            if (lo > hi - kMinDbSpan) {
                if (maxOnly) {
                    hi = lo + kMinDbSpan;
                } else {
                    lo = hi - kMinDbSpan;
                }
                res.flags |= FOXAPI_RESULT_CLAMPED;
            }
            dbMin_ = lo;
            dbMax_ = hi;
            host_.onDisplayRange(dbMin_, dbMax_);
            res.applied[0] = dbMin_;
            res.applied[1] = dbMax_;
            return res;
        }
        case FOXAPI_OP_SET_BAND_PLAN:
            if (text == bandPlanSelection_) { return refuse(FOXAPI_NO_CHANGE, "already that plan"); }
            bandPlanSelection_ = text;
            loadBandPlan();
            return res;

        // --- the source and the radio ------------------------------------------------
        case FOXAPI_OP_SCAN_DEVICES:
            // The native list always refreshes (it opens nothing); the
            // SoapySDR scan goes through its own gate, which defers while a
            // radio is open and says why once.
            scanNative();
            scanSoapy();
            return res;
        case FOXAPP_OP_SCAN_DEVICES_ON_OPEN:
            // The Source list opening: the lazy first SoapySDR scan, or the
            // whole scan a partial one left owed.
            scanNative();
            if (!soapyScanned_ || (soapyScanPartial_ && !soapyScanGated())) { scanSoapy(); }
            return res;
        case FOXAPP_OP_SET_NETWORK_USRP_SCAN:
            lookForNetworkUsrps_ = on;
            if (lookForNetworkUsrps_) { scanSoapy(); }
            return res;
        case FOXAPI_OP_SELECT_SOURCE:
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no source named"); }
            if (!selectSourceById(text)) {
                return refuse(FOXAPI_NOT_FOUND, "no scanned device matches that id");
            }
            return res;
        case FOXAPI_OP_SET_SAMPLE_RATE: {
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            // The refusal's reason, or a coercion's, lands on the line; the
            // Rate combo follows the READBACK.
            const cascade::gui::RateSetOutcome set =
                cascade::gui::applySourceRate(*device_, c.num[0], sourceError_);
            sourceError_ = set.sourceError;
            if (!set.ok) { return refuse(FOXAPI_FAILED, "the radio refused the rate"); }
            deviceRateIndex_ =
                nearestIndex(deviceRatesHz_, pipeline_.activeSource().sampleRateHz());
            followInputRate();
            res.applied[0] = pipeline_.activeSource().sampleRateHz();
            return res;
        }
        case FOXAPI_OP_SET_GAIN: {
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            if (!device_->setGainDb(text, c.num[0])) {
                sourceError_ = device_->lastError();
                return refuse(FOXAPI_FAILED, "the radio refused the gain");
            }
            // THE READBACK, not the request: every one of these radios
            // quantises, and a slider left on the request would be a lie.
            for (std::size_t i = 0; i < deviceGainNames_.size(); ++i) {
                if (deviceGainNames_[i] == text && i < deviceGainsDb_.size()) {
                    deviceGainsDb_[i] = static_cast<float>(device_->gainDb(text));
                    res.applied[0] = deviceGainsDb_[i];
                }
            }
            return res;
        }
        case FOXAPP_OP_SET_GAIN_NO_READBACK: {
            // The radar scope's GAIN knob: the knob keeps the figure it asked
            // for (a divergence from SET_GAIN kept as it was).
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            for (std::size_t i = 0; i < deviceGainNames_.size() && i < deviceGainsDb_.size(); ++i) {
                if (deviceGainNames_[i] != text) { continue; }
                deviceGainsDb_[i] = static_cast<float>(c.num[0]);
                if (!device_->setGainDb(text, static_cast<double>(deviceGainsDb_[i]))) {
                    sourceError_ = device_->lastError();
                }
                res.applied[0] = deviceGainsDb_[i];
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no such gain stage");
        }
        case FOXAPI_OP_SET_DEVICE_AGC:
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            if (!deviceAgcSupported_) { return refuse(FOXAPI_UNSUPPORTED, "no automatic gain"); }
            if (!device_->setAutoGain(on)) {
                sourceError_ = device_->lastError();
                return refuse(FOXAPI_FAILED, "the radio refused");
            }
            deviceAgc_ = on;
            return res;
        case FOXAPI_OP_SET_ANTENNA:
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            if (!device_->setAntenna(text)) {
                sourceError_ = device_->lastError();
                return refuse(FOXAPI_FAILED, "the radio refused the antenna");
            }
            deviceAntenna_ = device_->antenna();  // readback, not the request
            return res;
        case FOXAPI_OP_SET_BIAS_TEE:
            // The Source panel's box and the deck's key both end here, after
            // the key's own question has been answered (biasKeyPressed).
            switchBiasTee(on);
            return res;
        case FOXAPI_OP_SET_DEVICE_OPTION: {
            // The RSP's notches and HDR, the RX888's ADC pair: request, then
            // show the driver's READBACK.
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            if (text == "rf_notch" && deviceRfNotchPresent_) {
                deviceRfNotch_ = on;
                withRfNotch(device_, [this](auto& d) {
                    if (!d.setRfNotch(deviceRfNotch_)) { sourceError_ = d.lastError(); }
                    deviceRfNotch_ = d.rfNotch();
                    return true;
                });
            } else if (text == "dab_notch" && deviceDabNotchPresent_) {
                deviceDabNotch_ = on;
                withDabNotch(device_, [this](auto& d) {
                    if (!d.setDabNotch(deviceDabNotch_)) { sourceError_ = d.lastError(); }
                    deviceDabNotch_ = d.dabNotch();
                    return true;
                });
            } else if (text == "hdr" && deviceHdrPresent_) {
                deviceHdr_ = on;
                withHdrMode(device_, [this](auto& d) {
                    if (!d.setHdrMode(deviceHdr_)) { sourceError_ = d.lastError(); }
                    deviceHdr_ = d.hdrMode();
                    return true;
                });
            } else if (text == "dither" && deviceAdcSwitchesPresent_) {
                deviceDither_ = on;
                withAdcSwitches(device_, [this](auto& d) {
                    if (!d.setDither(deviceDither_)) { sourceError_ = d.lastError(); }
                    deviceDither_ = d.dither();
                    return true;
                });
            } else if (text == "randomiser" && deviceAdcSwitchesPresent_) {
                deviceRandomiser_ = on;
                withAdcSwitches(device_, [this](auto& d) {
                    if (!d.setRandomiser(deviceRandomiser_)) { sourceError_ = d.lastError(); }
                    deviceRandomiser_ = d.randomiser();
                    return true;
                });
            } else {
                return refuse(FOXAPI_UNSUPPORTED, "this radio has no such switch");
            }
            return res;
        }
        case FOXAPP_OP_SET_CONVERTER: {
            if (c.ival[0] < 0 || c.ival[0] > 2) { return refuse(FOXAPI_OUT_OF_RANGE, "no such converter mode"); }
            // Not while a radio is being opened: the setting would land on
            // whichever radio happened to be installed at that instant (the
            // panel greys its controls for the same reason).
            if (deviceOpenPending_) { return refuse(FOXAPI_BUSY, "a radio is still opening"); }
            cascade::core::ConverterSetting s;
            s.mode = static_cast<cascade::core::ConverterMode>(c.ival[0]);
            s.loHz = c.num[0];
            s.inverted = c.ival[1] != 0;
            changeConverter(s);
            return res;
        }
        case FOXAPP_OP_SOUNDCARD_IQ_CENTRE:
            // The sound card panel's I/Q centre: only a record of where the
            // external receiver is tuned, so a card already running IN I/Q
            // MODE takes it at once and the whole receiver follows it; one
            // running in real mode keeps it for the next Open (the panel's
            // form holds it).
            if (!cascade::gui::soundCardCentreAppliesLive(sourceKind_ == "soundcard", soundCardOpenPending_,
                                                          soundCardLive_.format)) {
                return refuse(FOXAPI_NO_CHANGE, "no I/Q sound card is running");
            }
            soundCardLive_.iqCentreHz = c.num[0];
            applyRetuneNow(c.num[0], false);
            return res;

        // --- the recorder ----------------------------------------------------------
        case FOXAPI_OP_RECORD_IQ:
            if (on) {
                if (iqRecorder_.recording()) { return refuse(FOXAPI_NO_CHANGE, "already recording"); }
                if (!startIqRecording()) { return refuse(FOXAPI_FAILED, "the recording could not start"); }
            } else {
                stopIqRecording();
            }
            return res;
        case FOXAPI_OP_RECORD_AUDIO:
            if (on) {
                if (audioRecorder_.recording()) { return refuse(FOXAPI_NO_CHANGE, "already recording"); }
                if (!startAudioRecording()) { return refuse(FOXAPI_FAILED, "the recording could not start"); }
            } else {
                stopAudioRecording();
            }
            return res;

        // --- bookmarks ------------------------------------------------------------------
        case FOXAPI_OP_BOOKMARK_ADD:
            addBookmarkHere(text);
            return res;
        case FOXAPI_OP_BOOKMARK_TUNE:
        case FOXAPI_OP_BOOKMARK_REMOVE:
        case FOXAPI_OP_BOOKMARK_FAVOURITE: {
            // BY ID, re-found in the LIVE list: the list may have moved since
            // the command was made (an import, a browser's add).
            const std::size_t i = freqMgr_.indexOfId(static_cast<std::uint64_t>(c.ival[0]));
            if (i >= freqMgr_.list().size()) { return refuse(FOXAPI_NOT_FOUND, "no such bookmark"); }
            if (c.op == FOXAPI_OP_BOOKMARK_TUNE) {
                const cascade::core::Bookmark b = freqMgr_.list()[i];
                tuneToBookmark(b);
            } else if (c.op == FOXAPI_OP_BOOKMARK_REMOVE) {
                freqMgr_.removeAt(i);
                saveBookmarks();
            } else {
                cascade::core::Bookmark b = freqMgr_.list()[i];
                b.favourite = c.ival[1] != 0;
                freqMgr_.updateAt(i, b);
                saveBookmarks();
            }
            return res;
        }
        case FOXAPI_OP_BOOKMARK_REMOVE_GROUP: {
            const std::size_t n = freqMgr_.removeGroup(text);
            bookmarkImportNote_ = cascade::core::formatText(tr("Removed %zu from \"%s\""), n, text.c_str());
            saveBookmarks();
            host_.onBookmarksChanged();
            res.applied[0] = static_cast<double>(n);
            return res;
        }
        case FOXAPP_OP_BOOKMARK_IMPORT_FILE:
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no file named"); }
            importBookmarkFile(text);
            return res;

        // --- the scanner -------------------------------------------------------------------
        case FOXAPI_OP_SCANNER_RUN:
            if (on) {
                // The range travels with the command and is kept as the
                // scanner's stored range; configure() sanitizes it (swaps a
                // reversed range, floors the step) whoever sent it.
                scanStartMhz_ = c.num[0] / 1.0e6;
                scanStopMhz_ = c.num[1] / 1.0e6;
                scanStepKhz_ = c.num[2] / 1.0e3;
                scanner_.configure(scannerParams());
                scanner_.start(host_.frameTimeS() * 1000.0);
                scannerHasExpected_ = false;
            } else {
                scanner_.stop();
            }
            return res;
        case FOXAPI_OP_SCANNER_SKIP:
            if (!scanner_.active()) { return refuse(FOXAPI_NO_CHANGE, "the scanner is not running"); }
            scanner_.skip();
            scannerHasExpected_ = false;
            return res;
        case FOXAPI_OP_SCANNER_CONFIG:
            scanDwellMs_ = c.num[0];
            scanHoldMs_ = c.num[1];
            scanResumeMs_ = c.num[2];
            scanListenMs_ = c.num[3];
            // A running scan takes the new timings at once (the panel's own
            // commit-on-deactivate rule).
            if (scanner_.active()) {
                scanner_.configure(scannerParams());
                scannerHasExpected_ = false;
            }
            return res;
        case FOXAPP_OP_SCANNER_RANGE:
            if ((c.ival[0] & 1) != 0) { scanStartMhz_ = c.num[0] / 1.0e6; }
            if ((c.ival[0] & 2) != 0) { scanStopMhz_ = c.num[1] / 1.0e6; }
            if ((c.ival[0] & 4) != 0) { scanStepKhz_ = c.num[2] / 1.0e3; }
            // Only the panel reconfigures a running scan; a browser's range
            // is stored for the next start, as it always was.
            if ((c.ival[0] & 8) != 0 && scanner_.active()) {
                scanner_.configure(scannerParams());
                scannerHasExpected_ = false;
            }
            return res;

        // --- decoders and plugins ------------------------------------------------------------
        case FOXAPI_OP_DECODER_START:
        case FOXAPI_OP_DECODER_STOP:
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no plugin named"); }
            setPluginStopped(text, c.op == FOXAPI_OP_DECODER_STOP);
            return res;
        case FOXAPI_OP_DECODER_STOP_ALL:
            for (const std::string& key :
                 cascade::gui::stopAllKeys(runnableDecoders(), pipeline_.running())) {
                setPluginStopped(key, true);
            }
            return res;
        case FOXAPP_OP_DECODER_STOP_LIST: {
            // THE KEYS THE USER WAS SHOWN: STOP ALL's from the frame it was
            // drawn, the mute dialog's from when it opened - never "whatever
            // is running now".
            const std::vector<std::string> keys = linesOf(text);
            if (c.ival[0] == 1) {
                stopMutingPlugins(keys);
            } else {
                for (const std::string& key : keys) { setPluginStopped(key, true); }
            }
            return res;
        }
        case FOXAPI_OP_PLUGIN_PRESET: {
            // RE-READ FROM THE PLUGIN, never trusted from the press: count()
            // and get() are asked again and the preset re-validated, so the
            // only numbers that reach the receiver are ones the plugin itself
            // just produced. Named by display name (the web remote) or by
            // module file name (the preset bars).
            for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
                if (lp.name != text && cascade::core::pluginKey(lp) != text) { continue; }
                const std::uint32_t n = (lp.preset != nullptr) ? lp.preset->count() : 0u;
                const auto idx = static_cast<std::uint32_t>(c.ival[0]);
                CascadePreset ps{};
                ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
                const bool fetchedOk = c.ival[0] >= 0 && lp.preset != nullptr && idx < n &&
                                       lp.preset->get(idx, &ps) == 1;
                if (!cascade::gui::presetRequestStillValid(true, n, kMaxPresetsPerPlugin, idx,
                                                            fetchedOk, ps)) {
                    return refuse(FOXAPI_NOT_FOUND, "no such preset");
                }
                applyPluginPreset(lp, ps);
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no such plugin");
        }
        case FOXAPP_OP_USER_PRESET_APPLY: {
            // One of the user's own, re-read by ordinal: forgotten since the
            // press is a no-op.
            for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
                if (cascade::core::pluginKey(lp) != text) { continue; }
                const std::vector<cascade::core::UserPreset> mine = userPresetsForPlugin(lp);
                if (c.ival[0] < 0 || static_cast<std::size_t>(c.ival[0]) >= mine.size()) {
                    return refuse(FOXAPI_NOT_FOUND, "no such preset");
                }
                const CascadePreset ps =
                    cascade::gui::userPresetToCascade(mine[static_cast<std::size_t>(c.ival[0])]);
                if (!cascade::gui::presetIsValid(ps)) { return refuse(FOXAPI_NOT_FOUND, "no such preset"); }
                applyPluginPreset(lp, ps);
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no such plugin");
        }
        case FOXAPI_OP_USER_PRESET_SAVE:
            applyUserPresetEdit({PendingUserPresetEdit::Op::Save, text, 0});
            return res;
        case FOXAPP_OP_USER_PRESET_FORGET_AT:
            if (c.ival[0] < 0) { return refuse(FOXAPI_OUT_OF_RANGE, "no such preset"); }
            applyUserPresetEdit(
                {PendingUserPresetEdit::Op::Forget, text, static_cast<std::size_t>(c.ival[0])});
            return res;
        case FOXAPP_OP_PLUGIN_AUTO_PRESET:
            maybeAutoPreset(text, c.ival[0] == 1 ? "window opened" : "started");
            return res;
        case FOXAPI_OP_PLUGIN_GRANT:
            if (c.ival[0] == 1) {
                setPluginTuneAllowed(text, c.ival[1] != 0);
            } else if (c.ival[0] == 2) {
                setPluginSettingsAllowed(text, c.ival[1] != 0);
            } else {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such grant");
            }
            return res;
        case FOXAPI_OP_PLUGIN_COMMAND:
            // Queued for the plugin to take with poll_command; nothing of the
            // plugin's runs here.
            if (c.ival[0] < 0 || !pluginUi_.api().pressCommand(text, static_cast<std::uint32_t>(c.ival[0]))) {
                return refuse(FOXAPI_NOT_FOUND, "no such command key");
            }
            return res;
        case FOXAPI_OP_PLUGIN_MUTE:
            for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
                if (cascade::core::pluginKey(lp) != text) { continue; }
                setPluginMutes(lp, on);
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no such plugin");
        case FOXAPI_OP_PLUGIN_RESCAN:
            rescanPlugins();
            return res;
        case FOXAPI_OP_STORE_FETCH:
            startCatalogFetch();
            return res;
        case FOXAPI_OP_STORE_INSTALL: {
            // The legal notice must be acknowledged explicitly, and the gate
            // is the ONE predicate the store window's FIT key uses too.
            for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
                if (catalog_[static_cast<std::size_t>(i)].id != text) { continue; }
                const std::string blocked = pluginInstallBlockedReason(i, on);
                if (!blocked.empty()) {
                    installError_ = blocked;
                    return refuse(FOXAPI_DENIED, "the install was refused");
                }
                startInstall(catalog_[static_cast<std::size_t>(i)]);
                return res;
            }
            // English, as every reason in installError_ is; the pages
            // translate it where they draw it (gui::trStoredReason).
            installError_ = FOX_TR_NOOP("no catalogue entry with that id; fetch the "
                                        "catalogue and try again");
            return refuse(FOXAPI_NOT_FOUND, "no catalogue entry with that id");
        }
        case FOXAPP_OP_STORE_UPDATE: {
            // The plan is looked up again rather than captured with the key:
            // planUpdates' entries point INTO catalog_.
            for (const cascade::core::PluginUpdate& u : plannedPluginUpdates()) {
                if (u.id != text) { continue; }
                startUpdate(u);
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no update planned for that plugin");
        }
        case FOXAPI_OP_STORE_REMOVE:
            removeInstalledPlugin(text);
            return res;
        case FOXAPP_OP_STORE_REMOVE_BLOCKED:
            removeBlockedPlugin(text);
            return res;
        case FOXAPI_OP_STORE_CANCEL:
            pluginRepo_.cancel();
            return res;
        case FOXAPI_OP_STORE_UPDATE_ALL:
            startAddAll(on);
            return res;

        // --- the patch page ---------------------------------------------------------------------
        case FOXAPI_OP_PATCH_RUN:
            if (on == patchRunning_) { return refuse(FOXAPI_NO_CHANGE, "already so"); }
            patchPressStart();
            return res;
        case FOXAPI_OP_PATCH_ALL_OFF:
            patchAllOff();
            return res;

        // --- the transmitter (the local key is not a command in stage 1) --------------
        case FOXAPI_OP_TX_OPEN:
            if (!text.empty()) { transmitArgs_ = text; }
            openTransmitRadio();
            if (!transmitter_.haveSink()) { return refuse(FOXAPI_FAILED, "the transmitter did not open"); }
            return res;
        case FOXAPI_OP_TX_CLOSE:
            closeTransmitRadio();
            return res;
        case FOXAPI_OP_TX_PTT:
            // THE REMOTE KEY (0.95.1): an assertion with a deadline, not a
            // switch - keyRemote() buys kRemotePttHoldMs and the browser has to
            // keep asking. A request that arrives while the page is shut is
            // dropped, never remembered.
            if (on) {
                if (transmitOpen_ && transmitter_.haveSink()) {
                    transmitter_.keyRemote();
                } else {
                    transmitter_.releaseRemote("there is no transmitter open");
                    return refuse(FOXAPI_NO_DEVICE, "there is no transmitter open");
                }
            } else {
                transmitter_.releaseRemote("the remote let go");
            }
            return res;
        case FOXAPI_OP_TX_SET_MODE:
            if (c.ival[0] < 0 || c.ival[0] >= cascade::dsp::kTxModeCount) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such transmit mode");
            }
            transmitModeIndex_ = static_cast<int>(c.ival[0]);
            transmitter_.setMode(cascade::dsp::txModeFromIndex(transmitModeIndex_));
            return res;
        case FOXAPI_OP_TX_SET_FREQUENCY:
            // Where a SPLIT transmitter goes; with SPLIT off it follows the
            // receiver's dial and this is kept for when it is turned on.
            transmitSplitHz_ = c.num[0];
            followTransmitFrequency();
            return res;
        case FOXAPI_OP_TX_SET_SPLIT:
            transmitSplit_ = on;
            // Going INTO split starts from the frequency the command carries
            // (the page sends the receiver's dial), so the first thing that
            // happens is never a jump; coming out follows the receiver again.
            if (transmitSplit_) { transmitSplitHz_ = c.num[0]; }
            followTransmitFrequency();
            return res;
        case FOXAPI_OP_TX_SET_POWER:
            transmitPowerDb_ = c.num[0];
            transmitter_.setPowerDb(transmitPowerDb_);
            if (transmitter_.sink() != nullptr) { transmitPowerDb_ = transmitter_.sink()->gainDb(); }
            res.applied[0] = transmitPowerDb_;
            return res;
        case FOXAPI_OP_TX_SET_INPUT: {
            if (c.ival[0] < 0 || c.ival[0] >= cascade::core::kTxInputCount) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such input");
            }
            if (micOpen_.inFlight()) { return refuse(FOXAPI_BUSY, "the microphone is still opening"); }
            transmitInputIndex_ = static_cast<int>(c.ival[0]);
            const cascade::core::TxInput in = cascade::core::txInputFromIndex(transmitInputIndex_);
            transmitter_.setInput(in);
            if (in == cascade::core::TxInput::Microphone && !transmitter_.audioIn().running()) {
                // Opened WHEN IT IS CHOSEN and not before, and NOT ON THIS
                // THREAD: micOpen_ runs waveInOpen on a worker and this waits
                // at most AudioOpen::kOpenBound under a watchdog pause.
                const cascade::gui::AudioOpen::Outcome outcome = micOpen_.request(-1);
                if (outcome == cascade::gui::AudioOpen::Outcome::Finished) {
                    if (!micOpen_.result().ok) {
                        transmitError_ = FOX_TR_NOOP("no microphone could be opened");
                    }
                } else {
                    transmitError_ = kAudioBusyNote;
                }
            }
            return res;
        }
        case FOXAPI_OP_TX_SET_TONE:
            transmitToneHz_ = c.num[0];
            transmitter_.setToneHz(transmitToneHz_);
            transmitToneHz_ = transmitter_.toneHz();
            res.applied[0] = transmitToneHz_;
            return res;
        case FOXAPI_OP_TX_SET_MONITOR:
            transmitMonitor_ = on;
            return res;

        // --- audio output, position, GPS --------------------------------------------------------
        case FOXAPI_OP_AUDIO_DEVICE:
            // ival[0]: the PortAudio device index the Sinks list carries.
            for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
                if (devices_[static_cast<std::size_t>(i)].index != c.ival[0]) { continue; }
                deviceIndex_ = i;
                // Through audioOpen_, never straight at the sink: waveOutOpen
                // has no timeout (the 0.96.4 field hang).
                (void)requestAudioOpen(static_cast<int>(c.ival[0]), false);
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no such audio device");
        case FOXAPI_OP_SET_POSITION:
            // REFUSED, never clamped, by the one rule (receiverPositionAcceptable).
            if (!applyReceiverPosition(c.num[0], c.num[1])) {
                return refuse(FOXAPI_OUT_OF_RANGE, "not a receiver position");
            }
            return res;
        case FOXAPI_OP_GPS:
            if (on) {
                if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no serial port named"); }
                gpsRefusal_.clear();
                gpsReader_.start({text, static_cast<int>(c.num[0]),
                                  cascade::core::GpsReader::kDefaultTimeoutS});
            } else {
                gpsReader_.stop();
            }
            return res;

        default:
            break;
    }
    return refuse(FOXAPI_UNSUPPORTED, "this build has no such operation");
}

// ONE REQUEST, TRANSLATED. The web remote, CAT and a plugin's queued controls
// used to run a private copy of what each field does; they now turn into the
// same commands a desktop widget sends, applied in the order the fields always
// were (net/control_ops.cpp), by applyCommand. GUI thread only.
void Engine::applyControlRequest(const cascade::net::ControlRequest& r) {
    cascade::net::ControlOpsContext ctx;
    // The browser's row numbers name the ids of the block /api/status is
    // serving NOW - taken from that block itself, the block current when this
    // request is APPLIED, so a row never lands on a bookmark /api/status is
    // not showing there. An entry removed since answers NOT_FOUND, and one
    // inserted since moves no other row. THIS READFULL() IS A SHORT BLOCKING
    // LOCK taken by the publishing thread itself (the GUI thread in stage 3a;
    // the engine's control thread from 3b), once per web bookmark request: a
    // reader holds that lock only for a refcount copy or a slot install, so
    // the wait is that long - but it is a wait, unlike publish().
    if (r.bookmarkTune.has_value() || r.bookmarkRemove.has_value()) {
        ctx.bookmarkIdByRow = receiverSnapshot_->readFull()->bookmarkIds;
    }
    ctx.scanStartHz = scanStartMhz_ * 1.0e6;
    ctx.scanStopHz = scanStopMhz_ * 1.0e6;
    ctx.scanStepHz = scanStepKhz_ * 1.0e3;
    for (const cascade::core::cmd::QueuedCommand& q : cascade::net::controlRequestToCommands(r, ctx)) {
        (void)applyCommand(q.c, q.longText);
    }
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
