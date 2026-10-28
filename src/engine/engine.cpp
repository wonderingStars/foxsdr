// engine.cpp - the Engine's machinery from gui/app_window.cpp, moved VERBATIM (engine
// extraction stage 3a, docs/engine-stage3.md): each definition is the
// window's, renamed AppWindow:: -> Engine::, with the few lines that reached
// into the window turned into calls on the host (engine_host.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include "core/patch_draft.hpp"
#include "core/serial_port.hpp"
#include "engine/add_all_plan.hpp"
#include "core/patch_io.hpp"
#include "core/patch_levels.hpp"
#include "core/patch_plan.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
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
#include "core/breadcrumb.hpp"
#include "core/frame_timing.hpp"
#include "core/damaged_file.hpp"
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
#include <timeapi.h>
#endif

#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/airspy_panel.hpp"
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
    : host_(&host),
      pipeline_(cascade::core::Pipeline::Config{kSampleRateHz, kFftSize, kAveragingAlpha,
                                                /*audioEnabled=*/true}),
      api_(std::make_unique<ApiHost>(*this, std::string())) {
    // THE RING A TRANSMITTING PLUGIN'S AUDIO ARRIVES THROUGH (host API level 2; stub campaign B13): the runner holds the writing end, the Transmitter the reading end, and neither holds
    // a pointer to the other. Harmless with no plugin: nothing writes, and nothing reads unless a plugin's key is the only key down.
    transmitter_.setPluginLink(txLink_);
    pluginRunner_.setTxLink(txLink_);
    // A directive posted wakes a front-end thread that is waiting for the pass (waitForPassCounter): the pass it waits for may be
    // waiting, in turn, for that thread to apply the directive. The lock is taken and dropped so a waiter between its check and
    // its wait cannot miss the notify.
    pluginLink_.setOnPost([this] {
        { std::lock_guard<std::mutex> lk(controlWakeMutex_); }
        passDoneCv_.notify_all();
    });
    // THE ENGINE OWNS THE WEB SERVER AND THE CAT LISTENER (stage 5b), and they answer from the engine's own publish. Installed once, before
    // any start (a server refuses to change a provider while it runs): the browser's status is the whole block of one publish; (stage 5d) the
    // web server holds ONE SESSION on the table while it runs - the browser's control is that session's submits, and the browser's own poll of
    // its status is that session's heartbeat (API 0.2: only an interface's heartbeat counts), never its PTT re-assertions - and (stage 5c)
    // every CAT connection gets a SESSION of its own: its figures are that session's read_state and its sets are that session's submits. They
    // run on the servers' threads and read nothing but the snapshot and the API host, which outlive them (servers_ is the last member:
    // ~Engine, and shutdownQuiesce before it, stop them first - which closes every session). The host is reached through api_ at the call:
    // configureApi replaces it, before any server starts. The bookmark rows of a browser's request name the ids of the Full block its status
    // is served from, read here at the post.
    ServerHost::Providers providers;
    providers.webStatus = [this] { return webStatusNow(); };
    providers.webSession = [this](const std::string& label) {
        return openWebSession(*api_, [this] { return receiverSnapshot_->readFull()->bookmarkIds; }, label);
    };
    providers.catSession = [this](const std::string& label) { return openCatSession(*api_, label); };
    // (stage 5e) The receiver plugins' pictures: the host's image feed owns the ONE accumulator that polls the decoders (a decoder offers a
    // picture once, so two pollers would split them), through the runner this engine owns.
    providers.pollImages = [this](std::vector<cascade::core::HostImage>& out) { pluginRunner_.pollImages(out); };
    servers_.attach(std::move(providers));
}

// What the web page's status reads: the whole block of one publish - figures and lists together - so a browser can never be shown two fields
// from different publishes. The lock inside readFull() is held for a reference count (or the install of a handed-over block); the publish
// only ever try-locks it (core/receiver_snapshot.hpp). (AppWindow::webStatusNow, moved here with the server.)
cascade::net::RadioStatus Engine::webStatusNow() const {
    const std::shared_ptr<const cascade::core::ReceiverSnapshot::Full> full = receiverSnapshot_->readFull();
    return cascade::net::composeRadioStatus(full->state, full->lists.get());
}

// (CAT's figures - frequency, offset, rate, mode, bandwidth, run state - are not read here any more: each connection reads them through
// the session it holds, the table's read_state; stage 5c deleted Engine::catStatusNow, and with it the Full-block fallback - the
// connection retries a read that overlapped a publish once and keeps its last good figures.)

void Engine::configureApi(const std::string& options) {
    api_ = std::make_unique<ApiHost>(*this, options);
    servers_.setHosting(api_->hostsServers());  // a hermetic test engine binds nothing (stage 5b)
    apiLists_.reset();  // the new host is handed a block on the next publish
    apiLive_.reset();   // ...and the patch's live readings (API 0.3 8.12)
}

void Engine::wakeControl() {
    {
        std::lock_guard<std::mutex> lk(controlWakeMutex_);
        controlWake_ = true;
        // The first wake since the last pass started: what the lag to the publish is measured from (engine/pass_stats.hpp).
        if (controlWakeAtNs_ == 0) {
            controlWakeAtNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }
    controlWakeCv_.notify_one();
    kickControlWait();
}

// The control loop's timer wait is not a condition variable's: a wake sets the event that wait also listens to. The flags the wake set
// (under controlWakeMutex_) are set BEFORE this, and the event stays signalled until the loop's wait takes it, so a wake that lands between
// the loop's check of the flags and its wait is not lost.
void Engine::kickControlWait() {
#ifdef _WIN32
    if (void* const ev = passWakeEvent_.load(std::memory_order_acquire); ev != nullptr) { (void)::SetEvent(static_cast<HANDLE>(ev)); }
#endif
}

// THE FRAME'S KICK (docs/engine-stage4.md 6.9). Called by the window, once a frame, on its own thread. Same protocol as a submit's wake - the
// flag under the mutex, then the notify and the event - but a flag of its own: a kick stamps no controlWakeAtNs_, so the submit-to-publish
// figure stays the submits'. Nothing happens unless the free-running loop is there to be woken.
void Engine::frameKick() {
    if (!controlRun_.load() || controlStepped_.load() || onControlThread()) { return; }
    const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    lastKickNs_.store(nowNs, std::memory_order_relaxed);  // the frames are flowing, whether or not this one starts a pass
    {
        // The gap is tested and the flag set under the lock the loop clears the flags and stamps passStartNs_ under, so a kick can never be
        // judged against the OLD pass's start and then set the flag after the new pass has taken its own (one spurious pass a flood).
        std::lock_guard<std::mutex> lk(controlWakeMutex_);
        if (const std::int64_t gapNs = kickGapNs_.load(std::memory_order_relaxed);
            gapNs > 0 && nowNs - passStartNs_.load(std::memory_order_relaxed) < gapNs) {
            passStats_.kicksDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        controlKick_ = true;
    }
    passStats_.kicks.fetch_add(1, std::memory_order_relaxed);
    controlWakeCv_.notify_one();
    kickControlWait();
}

// A front end that did not take the engine down itself (a headless one, a
// test) gets the same teardown, in the same order, here.
//
// SAFETY (engine/stage3b-pre, the Low item docs/engine-stage3.md OPEN
// (fallback teardown)): a WELL-BEHAVED external front end (gui::AppWindow)
// ALWAYS calls teardown() itself before engineHolder_ is destroyed
// (AppWindow::~AppWindow), which sets tornDown_ and makes this whole block
// dead code for it. The one way this block runs with an EXTERNAL host_ is
// the front end's OWN CONSTRUCTOR throwing before it ever reaches that
// point: engineHolder_ is declared FIRST in AppWindow (so destroyed LAST),
// which means by the time this destructor body runs, every AppWindow member
// declared AFTER engineHolder_ may already be gone - even though AppWindow's
// own destructor never runs at all (construction failed, so the object was
// never "born"). AppWindow's vtable is still AppWindow's throughout that
// unwind (its own destructor never started), so calling any EngineHost hook
// here would dispatch, through that still-valid vtable, straight into
// AppWindow's override - which then reads AppWindow's OWN data members,
// some of which the unwind has already destroyed. So: BEFORE running any
// teardown here, repoint host_ at the engine's OWN host. A headless engine
// already has host_ == &ownHost_ (this is then a no-op); a real AppWindow
// that reaches here is, by construction, one whose teardown() the front end
// never got to call - so it has never told the engine anything host_
// specific needs doing, and detaching is the only safe choice. Proven with
// tests/test_engine_fallback_teardown.cpp (a throwing constructor seam).
Engine::~Engine() {
    if (!tornDown_) {
        host_ = &ownHost_;
        // The front end's applier points into a window that is, by construction of this path, no longer there.
        setPluginUiApplier({});
        stopTransfers();
        teardown();
    }
#ifdef _WIN32
    // The control thread is gone (teardown, above or earlier): nothing waits on, or sets, these any more.
    if (void* const t = passTimer_.exchange(nullptr); t != nullptr) { (void)::CloseHandle(static_cast<HANDLE>(t)); }
    if (void* const e = passWakeEvent_.exchange(nullptr); e != nullptr) { (void)::CloseHandle(static_cast<HANDLE>(e)); }
#endif
}

namespace {

// Recording destination: %USERPROFILE%/Documents/SDR-recordings per spec.
// Computed once at construction; the directory itself is created by
// Recorder::start on the first take, never at startup. An unset USERPROFILE
// (deliberately stripped environment) falls back to a relative directory —
// the same "stay writable" philosophy as ConfigStore::defaultPath's ".".
std::string defaultRecordDir() {
    const char* home = std::getenv("USERPROFILE");
    if (home == nullptr || *home == '\0') { home = std::getenv("HOME"); }
    if (home == nullptr || *home == '\0') { return "SDR-recordings"; }
    return std::string(home) + "/Documents/SDR-recordings";
}

// The WatchdogPause the window's plugin rescan held (core/hang_watchdog.hpp):
// paused now, resumed on every way out of the scope - the watchdog of the
// thread the wait is on (Engine::pauseWaitWatchdog; OPEN 8).
class WaitWatchdogPause {
public:
    explicit WaitWatchdogPause(Engine& e) : e_(e) { e_.pauseWaitWatchdog(); }
    ~WaitWatchdogPause() { e_.resumeWaitWatchdog(); }
    WaitWatchdogPause(const WaitWatchdogPause&) = delete;
    WaitWatchdogPause& operator=(const WaitWatchdogPause&) = delete;

private:
    Engine& e_;
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


// SoapyAudio advertises every sound card on the machine as a SoapySDR device.
// They are not receivers: no tuner (centerFrequencyHz reads 0), no RF, and
// selecting one silently swaps your radio for a microphone input — which then
// gets persisted to config and restored on the next launch, so the real SDR
// appears to have "stopped being detected". They are filtered out of the
// Source list entirely, matching the policy --soapy-check already applies.
bool isAudioDriver(const std::string& args) {
    return args.find("driver=audio") != std::string::npos;
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

bool isDeviceScopedOp(std::uint32_t op) { return isDeviceScoped(op); }

namespace {

// The shortest decimal that reads back as `f`, as a double: 0.4f -> 0.4.
// No allocation (two stack buffers); used for the handful of float-kept
// figures the publish carries.
double floatAsDecimal(float f) {
    if (!std::isfinite(f)) { return static_cast<double>(f); }
    char buf[32];
    const std::to_chars_result t = std::to_chars(buf, buf + sizeof(buf), f);
    double d = static_cast<double>(f);
    if (t.ec == std::errc()) { (void)std::from_chars(buf, t.ptr, d); }
    return d;
}

// A scanner timing as the state publishes it (API 0.3 8.6): whole milliseconds, rounded to the
// nearest (a half goes up), 0 for a negative or NaN - which Scanner::configure floors the same
// way - and saturating at the top of a uint32.
std::uint32_t wholeMilliseconds(double ms) {
    if (!(ms >= 0.0)) { return 0u; }
    if (ms >= 4294967295.0) { return 0xFFFFFFFFu; }
    return static_cast<std::uint32_t>(std::floor(ms + 0.5));
}

// The bytes of `s` into a fixed field, cut at its size less the NUL, the rest
// of the field zeroed. RAW BYTES, not a UTF-8 cut: the RDS decoder passes the
// bytes at 0x80 and above through as it received them (dsp/rds.cpp,
// sanitizeChar), and the window drew them as they came. No allocation.
void copyFixed(char* dst, std::size_t cap, const std::string& s) {
    if (dst == nullptr || cap == 0) { return; }
    const std::size_t n = std::min(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    std::memset(dst + n, 0, cap - n);
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
    ctx.sdrModel = cascade::core::scrubModel(deviceModel_);
    // The crystal correction and how it is applied - never a frequency.
    ctx.ppm = ppmDiagText();
    // THE PATCH PAGE'S RADIOS, by driver kind and nothing else: the part of the
    // node's device key before its '|' (a generator is "siggen", a recording is
    // "iqfile"), so the key's arguments - a serial, a path - are never read into
    // the block. A fault on a patch radio otherwise reads as a fault with no radio
    // at all (the 0.99.59 `sdrplay` report: `source: siggen`, `device-open: no`).
    // One entry per RUNNING radio (patchRadios_), not per node on the canvas.
    for (const auto& running : patchRadios_) {
        const cascade::core::patch::Node* node = patchGraph_.find(running.first);
        const std::string key = node != nullptr ? node->device : std::string();
        const std::string driver = cascade::core::patch::deviceDriver(key);
        ctx.patchRadioKinds.push_back(driver.empty() ? key.substr(0, key.find('|')) : driver);
    }
    std::sort(ctx.patchRadioKinds.begin(), ctx.patchRadioKinds.end());
    // The other software's DLLs in this process, by file name (0.99.69): what the watch has
    // seen at start and since. In the one builder, so a bundle and a fault report say the
    // same thing about it.
    ctx.foreignModules = foreignModules_.contextValue();
    // The sound path: the same facts the Sinks panel's chip and the rail's
    // volume, mute and squelch controls show, none of them a frequency.
    {
        cascade::sink::AudioOut& out = pipeline_.audio();
        const bool opening = audioOpen_.inFlight();
        cascade::core::DiagAudio& a = ctx.audio;
        a.known = true;
        a.opening = opening;
        if (!opening) {
            a.everOpened = out.everOpened();
            a.alive = a.everOpened && out.streamAlive();
            a.channels = out.channels();
            a.hostApi = out.openedHostApi();
        }
        a.restarts = audioRecoveries_ > 0 ? static_cast<unsigned>(audioRecoveries_) : 0u;
        a.volumePercent = static_cast<int>(std::lround(static_cast<double>(volume_) * 100.0));
        a.mutedByUser = userMuted_;
        a.mutedByPlugin = !muteSubjectText().empty();
        a.mutedByTransmit = transmitter_.transmitting() && !transmitMonitor_;
        a.squelchDb = static_cast<double>(squelchDb_);
        a.squelchOpen = pipeline_.squelchOpen();
        a.signalDb = static_cast<double>(pipeline_.signalPowerDb());
    }
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

// What the diagnostics context is made of, as one hash (the bytes of the same fields refreshDiagContext reads). No allocation.
std::uint64_t Engine::diagContextInputs() {
    std::uint64_t h = 1469598103934665603ull;
    const auto bytes = [&h](const void* p, std::size_t len) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < len; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    const auto text = [&](const std::string& s) {
        bytes(s.data(), s.size());
        const unsigned char end = 0xFF;
        bytes(&end, 1);
    };
    bytes(&modeIndex_, sizeof(modeIndex_));
    text(sourceKind_);
    const double rate = pipeline_.activeSource().sampleRateHz();
    bytes(&rate, sizeof(rate));
    const bool open = (device_ != nullptr);
    bytes(&open, sizeof(open));
    text(deviceModel_);
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded) { continue; }
        text(p.name);
        text(p.version);
    }
    for (const auto& running : patchRadios_) { bytes(&running.first, sizeof(running.first)); }
    {
        const std::uint64_t foreign = foreignModules_.contextHash();
        bytes(&foreign, sizeof(foreign));
    }
    // The crystal correction's line in the block (ppmDiagText), from the members it is built from: no string is made here.
    bytes(&ppmCorrectionOn_, sizeof(ppmCorrectionOn_));
    bytes(&ppmRadioApplied_, sizeof(ppmRadioApplied_));
    text(ppmError_);
    for (const auto& kv : ppmValues_) {
        text(kv.first);
        bytes(&kv.second, sizeof(kv.second));
    }
    // The sound path's facts move with the volume, the mutes, the squelch and the sink's life (the level and the squelch lamp are
    // measurements: rounded to the whole decibel the block prints, and the lamp itself).
    {
        const bool alive = audioOpen_.inFlight() ? false : pipeline_.audio().streamAlive();
        bytes(&alive, sizeof(alive));
        const bool opening = audioOpen_.inFlight();
        bytes(&opening, sizeof(opening));
        const int vol = static_cast<int>(std::lround(static_cast<double>(volume_) * 100.0));
        bytes(&vol, sizeof(vol));
        bytes(&userMuted_, sizeof(userMuted_));
        const bool pluginMuted = !muteSubjectText().empty();
        bytes(&pluginMuted, sizeof(pluginMuted));
        const bool txMuted = transmitter_.transmitting() && !transmitMonitor_;
        bytes(&txMuted, sizeof(txMuted));
        const int sq = static_cast<int>(std::lround(static_cast<double>(squelchDb_)));
        bytes(&sq, sizeof(sq));
        const bool sqOpen = pipeline_.squelchOpen();
        bytes(&sqOpen, sizeof(sqOpen));
        const int sig = static_cast<int>(std::lround(static_cast<double>(pipeline_.signalPowerDb())));
        bytes(&sig, sizeof(sig));
        bytes(&audioRecoveries_, sizeof(audioRecoveries_));
    }
    return h;
}

void Engine::pumpDiagContext() {
    // The foreign-module watch starts with the first pass (idempotent) and writes a line for each module that arrives afterwards.
    foreignModules_.start();
    foreignModules_.poll();
    const auto now = std::chrono::steady_clock::now();
    if (diagContextHave_ && now - diagContextAt_ < std::chrono::seconds(1)) { return; }
    diagContextAt_ = now;
    const std::uint64_t inputs = diagContextInputs();
    if (diagContextHave_ && inputs == diagContextHash_) { return; }
    diagContextHave_ = true;
    diagContextHash_ = inputs;
    ++diagContextRenders_;
    refreshDiagContext();
}

void Engine::closeAudioMinute(std::size_t ringFramesNow) {
    cascade::sink::AudioOut& out = pipeline_.audio();
    const std::uint64_t underrunsNow = out.underruns();
    const std::uint64_t primingNow = out.primingCallbacks();
    // The minute's starved callbacks. The counter is monotonic; the guard is for
    // a sink that was replaced under an engine that kept its mark.
    const std::uint64_t starved =
        underrunsNow >= audioUnderrunsAtLogStart_ ? underrunsNow - audioUnderrunsAtLogStart_ : 0;
    if (starved > 0) {
        const double rateHz = cascade::core::Pipeline::kAudioRateHz;
        const double lowWaterMs = 1000.0 * static_cast<double>(audioRingLowWaterFrames_) / rateHz;
        const double capacityMs = 1000.0 * static_cast<double>(out.ringCapacityFrames()) / rateHz;
        cascade::core::diagLogf(
            "audio: %llu starved callbacks in the last minute (%llu priming), "
            "ring low water %.0f ms of %.0f ms",
            static_cast<unsigned long long>(starved),
            static_cast<unsigned long long>(primingNow - audioPrimingAtLogStart_), lowWaterMs,
            capacityMs);
    }
    audioUnderrunsAtLogStart_ = underrunsNow;
    audioPrimingAtLogStart_ = primingNow;
    audioRingLowWaterFrames_ = ringFramesNow;  // next minute's mark starts here

    // THE BUFFER THAT DEEPENS ITSELF (0.99.73) AND EASES BACK (0.99.83). The
    // 12CF report - 110 to 126 starved callbacks every minute at a fixed 120 ms
    // - is the case: when a minute closes with three or more, the lead steps up
    // one rung (sink::nextAudioLead, which is also where the ceiling and "a
    // fixed setting never steps" live); a quarter hour of played, starvation-free
    // minutes steps it back down one (sink::audioLeadAfterQuietMinutes, where
    // the asymmetry is argued). Either step is one relaxed store the audio
    // callback reads at its next re-prime; this is the engine's thread and the
    // callback is never involved. A raise is counted once a session as
    // `recovered.audiolead`, so the effect of the release can be measured; the
    // way back down is not a recovery and counts nothing.
    const auto sayRaised = [](int toMs, std::uint64_t starvedCount, const char* who) {
        const int shown = static_cast<int>(std::min<std::uint64_t>(starvedCount, 1000000u));
        cascade::core::diagLogf("audio: buffer raised to %d ms after %d starved callbacks in a minute%s",
                                toMs, shown, who);
        cascade::core::health::noteRecovered(cascade::core::health::Recovered::AudioLead);
        return shown;
    };
    const int leadMs = cascade::sink::audioLeadMs(out.leadFrames());
    const int nextMs = cascade::sink::nextAudioLead(starved, leadMs, audioBufferMs_);
    if (nextMs > leadMs) {
        out.setLeadFrames(cascade::sink::audioLeadFrames(nextMs));
        audioLeadNote_ = AudioLeadNote::kRaised;
        audioLeadNoteMs_ = nextMs;
        audioLeadNoteCount_ = sayRaised(nextMs, starved, "");
        audioLeadQuietMinutes_ = 0;   // whatever run there was ended in a raise
        // REMEMBERED FOR THE NEXT LAUNCH: a computer that fell behind this often is slow every time,
        // and starting at 120 ms again costs the same bad minutes at every start. Only the
        // receiver's own raise is kept (the patch speakers are seeded from it when they are made),
        // and only in AUTOMATIC - nextAudioLead never raises a fixed setting. Saved by the normal
        // config save, as audioBufferAutoMs; audioBufferMs itself stays 0, because a raise is the
        // session's and not a choice.
        audioBufferAutoMs_ = nextMs;
    } else {
        // THE WAY BACK DOWN (0.99.83). A quiet minute is one the sink PLAYED
        // through (primed at its close - an idle sink proves nothing; stopping
        // the radio starves the last callback, which resets the run anyway) and
        // starved nobody. A quarter hour of them in a row lowers the lead one
        // rung, and the remembered depth with it - at the 120 ms floor there is
        // nothing left to remember, so the memory is 0 again. Why a lead comes
        // back down at all, against the old "a machine that fell behind once
        // will again": sink/audio_out.hpp, the policy comment.
        audioLeadQuietMinutes_ =
            starved == 0 && out.primed()
                ? std::min(audioLeadQuietMinutes_ + 1, cascade::sink::kAudioLeadQuietMinutesToStepDown)
                : 0;
        const int downMs = cascade::sink::audioLeadAfterQuietMinutes(audioLeadQuietMinutes_, leadMs,
                                                                     audioBufferMs_);
        if (downMs < leadMs) {
            out.setLeadFrames(cascade::sink::audioLeadFrames(downMs));
            cascade::core::diagLogf("audio: buffer lowered to %d ms after %d quiet minutes", downMs,
                                    audioLeadQuietMinutes_);
            audioLeadNote_ = AudioLeadNote::kLowered;
            audioLeadNoteMs_ = downMs;
            audioLeadNoteCount_ = audioLeadQuietMinutes_;
            audioLeadQuietMinutes_ = 0;   // the next rung needs a quarter hour of its own
            audioBufferAutoMs_ = downMs == 120 ? 0 : downMs;
        }
    }

    // EACH PATCH SPEAKER THAT IS A SOUND DEVICE has a sink of its own, a matcher of
    // its own and so a count of its own: the same rule on its minute. A speaker
    // seen for the first time only sets its baseline - what it starved before the
    // engine looked is not this minute's. Files have no buffer (leadMs() is 0).
    std::set<cascade::core::patch::NodeId> speakers;
    for (const auto& [node, dest] : patchDests_) {
        if (!dest || dest->leadMs() <= 0) { continue; }
        speakers.insert(node);
        const std::uint64_t total = dest->starvedCallbacks();
        const auto was = patchStarvedAtMinute_.find(node);
        const std::uint64_t before = was == patchStarvedAtMinute_.end() ? total : was->second;
        const std::uint64_t minute = total >= before ? total - before : 0;
        patchStarvedAtMinute_[node] = total;
        const int speakerMs = dest->leadMs();
        const int speakerNext = cascade::sink::nextAudioLead(minute, speakerMs, audioBufferMs_);
        if (speakerNext > speakerMs) {
            dest->setLeadMs(speakerNext);
            (void)sayRaised(speakerNext, minute, " (patch speaker)");
            patchQuietMinutes_[node] = 0;
        } else {
            // The same way back down, on the speaker's own run. A dest cannot
            // say whether it played (no primed() crosses the AudioDest seam),
            // so its quiet minute is only "starved nobody" - a cheaper test is
            // acceptable here because nothing about a speaker is persisted and
            // a wrong step costs one bad minute on that speaker alone.
            int& quiet = patchQuietMinutes_[node];
            quiet = minute == 0
                        ? std::min(quiet + 1, cascade::sink::kAudioLeadQuietMinutesToStepDown)
                        : 0;
            const int speakerDown =
                cascade::sink::audioLeadAfterQuietMinutes(quiet, speakerMs, audioBufferMs_);
            if (speakerDown < speakerMs) {
                dest->setLeadMs(speakerDown);
                cascade::core::diagLogf(
                    "audio: buffer lowered to %d ms after %d quiet minutes (patch speaker)",
                    speakerDown, quiet);
                quiet = 0;
            }
        }
    }
    for (auto it = patchStarvedAtMinute_.begin(); it != patchStarvedAtMinute_.end();) {
        it = speakers.count(it->first) != 0 ? std::next(it) : patchStarvedAtMinute_.erase(it);
    }
    for (auto it = patchQuietMinutes_.begin(); it != patchQuietMinutes_.end();) {
        it = speakers.count(it->first) != 0 ? std::next(it) : patchQuietMinutes_.erase(it);
    }
}

void Engine::pollDspMinute() {
    // The bundle's copy of the counters (core::setRingCounters): every pass, two loads and two stores, drop count first.
    {
        const std::uint64_t droppedNow = pipeline_.ringDroppedSamples();
        const std::uint64_t readNow = pipeline_.sourceSamplesRead();
        cascade::core::setRingCounters(droppedNow, readNow);
    }
    const double now = clockS();
    if (now - lastDspLogSec_ >= 60.0) {
        lastDspLogSec_ = now;
        closeDspMinute();
    }
}

void Engine::closeDspMinute() {
    // THE DROP COUNT FIRST. The source thread adds a read to its count before the
    // write that may drop part of it, so reading in this order never finds a drop
    // whose read is not yet counted. Both counters are monotonic and the pipeline
    // lives as long as the engine; the guards are for marks that ever got ahead.
    const std::uint64_t droppedNow = pipeline_.ringDroppedSamples();
    const std::uint64_t readNow = pipeline_.sourceSamplesRead();
    const std::uint64_t dropped = droppedNow >= dspDroppedAtMinute_ ? droppedNow - dspDroppedAtMinute_ : 0;
    const std::uint64_t read = readNow >= dspReadAtMinute_ ? readNow - dspReadAtMinute_ : 0;
    dspDroppedAtMinute_ = droppedNow;
    dspReadAtMinute_ = readNow;
    // QUIET UNLESS SOMETHING WAS LOST, like the audio digest beside it. 94
    // characters at the widest a uint64 can make it - inside the log's line
    // (DiagLog::kLineBytes), which a cut line would be masked whole by the scrub.
    if (dropped == 0) { return; }
    const std::string share = cascade::core::droppedShareText(dropped, read);
    cascade::core::diagLogf("dsp: %llu samples dropped between the source and the DSP in the last minute (%s)",
                            static_cast<unsigned long long>(dropped),
                            share.empty() ? "share unknown" : share.c_str());
}

void Engine::setAudioBufferMs(int ms) {
    if (!cascade::sink::validAudioBufferSetting(ms)) { ms = 0; }
    const bool changed = ms != audioBufferMs_;
    audioBufferMs_ = ms;
    cascade::sink::AudioOut& out = pipeline_.audio();
    // A fixed value is the lead from now on (and applying it again is harmless:
    // a restore from the saved file does it before the first prime). AUTOMATIC
    // starts again from the default - but only when it is a CHANGE to automatic: a
    // restore that names automatic for an engine that is already automatic must not
    // undo what the session has had to do.
    int toMs = 0;
    if (ms != 0) {
        toMs = ms;
    } else if (changed) {
        toMs = cascade::sink::audioLeadMs(cascade::sink::AudioOut::kPrimeFrames);
        // CHOOSING AUTOMATIC (from a fixed value) IS A FRESH START, and forgets the depth it had
        // remembered across launches: the person asking for it is asking not to be held at what an
        // earlier session had to do. Picking a fixed value does NOT forget it (it is kept, and
        // ignored while the fixed value is in force).
        audioBufferAutoMs_ = 0;
    }
    if (toMs != 0) {
        out.setLeadFrames(cascade::sink::audioLeadFrames(toMs));
        audioLeadNote_ = AudioLeadNote::kNone;   // the notice was about automatic mode's last step
        audioLeadNoteMs_ = 0;
        audioLeadNoteCount_ = 0;
        audioLeadQuietMinutes_ = 0;              // a chosen lead starts a quiet run of its own
        patchQuietMinutes_.clear();
        for (const auto& entry : patchDests_) {
            if (entry.second && entry.second->leadMs() > 0) { entry.second->setLeadMs(toMs); }
        }
    }
}

int Engine::audioLeadNowMs() {
    return cascade::sink::audioLeadMs(pipeline_.audio().leadFrames());
}

std::string Engine::audioLeadNoteFact() const {
    const char* kind = nullptr;
    switch (audioLeadNote_) {
        case AudioLeadNote::kNone: return std::string();
        case AudioLeadNote::kRaised: kind = "raised"; break;
        case AudioLeadNote::kRestored: kind = "restored"; break;
        case AudioLeadNote::kLowered: kind = "lowered"; break;
    }
    return std::string(kind) + " " + std::to_string(audioLeadNoteMs_) + " " + std::to_string(audioLeadNoteCount_);
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

    const double now = clockS();

    // Once-a-minute starvation digest. Silent unless something actually
    // starved in the window just closed — same "quiet unless it has news"
    // posture as the recorder's own status lines, and the only way a number
    // nobody watches (this goes to the log, not the screen) stays worth
    // reading when it does fire.
    if (now - lastAudioLogSec_ >= 60.0) {
        lastAudioLogSec_ = now;
        closeAudioMinute(ringFrames);
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
    // THE OUTPUT STOPPED AND CAME BACK ON ITS OWN: counted once a session (the watchdog checks once a second, so a device that flaps would
    // otherwise count every flap; audioRecoveries_ above is the number the Sinks panel shows).
    cascade::core::health::noteRecovered(cascade::core::health::Recovered::Audio);
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
        // Neither the generator nor a recording (0.99.40) is on any bus.
        if (cascade::core::patch::isGeneratorKey(n->device) ||
            cascade::core::patch::isIqFileKey(n->device)) {
            continue;
        }
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
    // SAID, so a log shows when the vendor probe ran and a test can show when
    // it did not (tests/test_main_view: never for the view opening itself).
    cascade::core::diagLogf("soapy: device scan started%s", whole ? "" : " beside open radios");
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
    soapyScanUhdNoImages_ = std::make_shared<bool>(false);
    soapyScanUsrpOnUsb_ = std::make_shared<bool>(false);
    const std::shared_ptr<std::vector<std::string>> absentOut = soapyScanAbsent_;
    const std::shared_ptr<bool> uhdNoImagesOut = soapyScanUhdNoImages_;
    const std::shared_ptr<bool> usrpOnUsbOut = soapyScanUsrpOnUsb_;
    // enumerateIsolated() never throws and is simply empty on a machine with no
    // vendor modules; this is also the hot-plug refresh path. Called directly
    // rather than through SoapySource::enumerate() because that signature
    // promises a list and nothing more, and this caller also needs the scan's
    // uhdImagesMissing answer for the hint under the Source list.
    const auto scanHook = testHooks_.soapyScan;  // null in every build of the application
    soapyScanFuture_ = std::async(std::launch::async, [skip, namedSoapyArgs, lookForNetworkUsrps,
                                                       absentOut, uhdNoImagesOut, usrpOnUsbOut, scanHook] {
        if (scanHook != nullptr) { return scanHook(); }
        std::vector<cascade::usb::UsbId> present;
        const bool listed = cascade::usb::presentUsbIds(present);
        std::vector<cascade::gui::UsbVidPid> ids;
        for (const cascade::usb::UsbId& id : present) { ids.push_back({id.vid, id.pid}); }
        const std::vector<std::string> absent = cascade::gui::soapyDriversWithNoHardware(
            ids, listed, namedSoapyArgs, lookForNetworkUsrps);
        // Handed back for the hints under the Source list; the future being
        // ready is what makes them safe to read there.
        *absentOut = absent;
        *usrpOnUsbOut = cascade::gui::anyUsrpUsbId(ids);
        cascade::source::EnumOptions options;
        options.skipDrivers = skip;
        options.absentDrivers = absent;
        cascade::source::EnumResult r = cascade::source::enumerateIsolated(options);
        *uhdNoImagesOut = r.uhdImagesMissing;
        return std::move(r.devices);
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
        // ...and whether UHD said it has no firmware images while a USRP sat
        // on the bus, for the hint under the Source list and the radio-setup
        // page's images finding (2026-10-21).
        soapyUhdNoImages_ = soapyScanUhdNoImages_ && *soapyScanUhdNoImages_;
        soapyUsrpOnUsb_ = soapyScanUsrpOnUsb_ && *soapyScanUsrpOnUsb_;
        soapyScanUhdNoImages_.reset();
        soapyScanUsrpOnUsb_.reset();
        // THE DIAGNOSTICS THE SOURCE SECTION SAYS A LANDED SCAN WITH, taken here, once, and not asked of the
        // runtime by every frame that draws them (API 0.3 8.10). Only a WHOLE scan that found nothing is
        // evidence of "no radio hardware"; a scan beside an open radio left drivers out on purpose.
        takeScanDiagnostics(!soapyScanPartial_ && soapyDevices_.empty());
        // A SCAN THAT FOUND NO RADIO AT ALL, counted anonymously (0.99.64, core/health_events.hpp): the whole-bus scan, not one that left some
        // drivers out beside an open radio, and with the native list empty too. The ledger keeps one a session, so a Refresh that finds
        // nothing again is not a second.
        if (!soapyScanPartial_ && soapyDevices_.empty() && nativeDevices_.empty()) {
            cascade::core::health::noteScanNone();
        }
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
        // THE REASON CLASS, for the log line below and for the retry (0.99.92): read off the driver's own words here, before the sentence is
        // wrapped.
        const cascade::core::health::RadioReason whyReason = cascade::core::health::classifyRadioOpen(r.kind, why);
        const std::string whyClass = cascade::core::health::radioReasonWord(whyReason);
        // A TRY AT THE SAVED RADIO (pollSavedRadioRetry) THAT FAILED: busy again keeps the retries going on their schedule; anything else ends
        // them, and the ordinary message stands - waiting does not cure an unplugged dongle. It is the SAME incident as the start-up failure
        // already counted, so a busy-again try adds nothing to the anonymous count; a different reason is a new fact and is counted.
        const bool retryBusyAgain = r.savedRadioRetry && whyReason == cascade::core::health::RadioReason::Busy;
        if (r.savedRadioRetry) {
            savedRadioRetryBusy_ = retryBusyAgain;
            savedRadioRetryLastFailSec_ = retryClockS();
            if (retryBusyAgain && savedRadioRetryAttempts_ >= 4) {
                cascade::core::diagWarnf(
                    "source: the saved radio is still in use by another program after 4 more "
                    "tries - giving up");
            }
        }
        // A RADIO THAT WOULD NOT OPEN, COUNTED ANONYMOUSLY (0.99.64,
        // core/health_events.hpp): the driver kind and one of eight reason
        // classes read off what the driver said - and nothing of what it said.
        // Not for a result the user had already moved on from (returned above).
        if (!retryBusyAgain) { cascade::core::health::noteRadioFail(r.kind, why); }
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
                "source: %s (%s) did not open (%s) - the receiver is on the signal generator%s",
                model.c_str(), r.kind.c_str(), whyClass.c_str(),
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
    //
    // A RECOVERY REOPEN DOES NOT PUT THE REMEMBERED AUTO GAIN BACK (the last
    // argument): it is the same radio to the user, whose own state a moment ago
    // (r.recoveryAgc) is applied below, after the gains, exactly as before.
    adoptDeviceMirrors(*r.dev, r.kind, r.args, r.requestRateHz, !r.recovery);
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
            // ...and written down for this radio again (0.99.73): it was on, and
            // the memory should say so even if nothing wrote it earlier.
            rememberAutoGain(*r.dev, r.args, true);
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
    if (r.savedRadioRetry) {
        // THE SAVED RADIO CAME FREE (0.99.92): said once, with the model and never the args, like every other source line.
        cascade::core::diagLogf("source: the saved radio came free - %s opened on attempt %d",
                                (r.kind == "soapy"
                                     ? cascade::core::sanitiseDevice(r.args)
                                     : modelFromNativeLabel(nativeLabelFor(r.kind, r.args)))
                                    .c_str(),
                                savedRadioRetryAttempts_);
    }
    savedRadioRetryBusy_ = false;
    ++sourceGen_;  // this install is itself a source change
    installSource(std::move(r.dev));
    sourceKind_ = r.kind;
    // A RADIO OPENED - the denominator of the failures above - and the watch for
    // its samples to arrive (0.99.64, core/health_events.hpp).
    if (r.kind != "siggen" && r.kind != "file") {
        cascade::core::health::noteRadioOpen(r.kind);
        healthWatchSamples(r.kind);
    }
    // (Master also counted the radio for the beta-tester usage record here. That record is the WINDOW's - a headless engine must never be able to send a tester report -
    // so the window reads the kind of the source in use from the DEVICES list instead: AppWindow::testerNoteFacts, stub campaign B15.)
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
            // ...and through its crystal correction (0.99.56): an RSP corrects
            // by retuning (SdrPlaySource drives no correction of its own), so
            // it is told the corrected frequency here and the later tune finds
            // it already there (ppmPreTellHz).
            if (radioHz > 0.0) {
                r.preTuneRadioHz = ppmPreTellHz(r.kind, r.args, false, radioHz);
            }
        }
    }
    // An Airspy's own memory travels with the request (see airspyAtOpen).
    r.airspyAtOpen.reset();
    if (r.kind == "airspy" || r.kind == "hydrasdr") {
        const auto mem = airspyMemory_.find(cascade::core::airspyRadioKey(r.args, r.kind));
        if (mem != airspyMemory_.end()) { r.airspyAtOpen = mem->second; }
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
        // AN AIRSPY's REMEMBERED STATE FIRST (0.99.41): its decimation decides
        // which rates the request below is matched against.
        if (r.airspyAtOpen.has_value()) {
            if (cascade::source::AirspySource* air = cascade::gui::asAirspy(dev.get())) {
                cascade::gui::airspyApplySetting(*r.airspyAtOpen, *air);
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
    const double now = clockS();
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

void Engine::pollSavedRadioRetry() {
    // Two loads on every pass that matters: nothing armed (every bounded run, and every session whose saved radio opened or failed any way
    // but BUSY) takes the first return.
    if (!savedRadioRetryBusy_) { return; }
    // A RADIO KIND ONLY: the remembered source can also be an I/Q file or a sound card, which are not what this waits for.
    if (!restoreKeep_.valid() || restoreKeep_.kind == "file" || restoreKeep_.kind == "soundcard" || restoreKeep_.kind == "siggen") { return; }
    const double now = retryClockS();
    if (!cascade::gui::savedRadioRetryDue(restoreKeep_.valid(), savedRadioRetryBusy_, savedRadioRetryAttempts_, device_ != nullptr,
                                          deviceOpenPending_, soapyScanPending_, now, savedRadioRetryLastFailSec_)) {
        return;
    }
    // Stamped BEFORE anything can fail, as pollSoapyRecovery does: the delay is counted from this try, and a try that never reaches the worker
    // is not repeated on the next pass.
    ++savedRadioRetryAttempts_;
    savedRadioRetryLastFailSec_ = now;
    cascade::core::diagLogf("source: trying the saved radio again (attempt %d of 4)", savedRadioRetryAttempts_);

    // THE REQUEST THE SOURCE LIST WOULD HAVE MADE for this radio (selectSource), and the restore's own prefer-native rule: a config that says
    // SoapySDR for a dongle this product drives itself opens natively, falling back to the Soapy args only if the native driver refuses the tuner.
    DeviceOpenResult req;
    req.kind = restoreKeep_.kind;
    req.args = (req.kind == "soapy") ? restoreKeep_.soapyArgs : restoreKeep_.nativeArgs;
    if (req.kind == "soapy") {
        if (const std::optional<cascade::source::NativeDeviceInfo> nat = cascade::gui::preferNativeFor("soapy", req.args, nativeDevices_)) {
            req.fallbackSoapyArgs = req.args;
            req.kind = nat->driver;
            req.args = nat->args;
        }
    }
    req.requestRateHz = restoreKeep_.sampleRateHz > 0.0 ? restoreKeep_.sampleRateHz : kSoapyRateHz[kSoapyRateDefaultIndex];
    // The combo row of this radio when this machine still lists it, as the restore finds it; -1 otherwise (the preview then names the live source).
    req.row = -1;
    if (req.kind != "soapy") {
        for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
            if (nativeDevices_[i].args == req.args) { req.row = kNativeRowBase + static_cast<int>(i); }
        }
    } else {
        for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
            if (soapyDevices_[i].args == req.args) { req.row = soapyRowBase() + static_cast<int>(i); }
        }
    }
    // The generator has been running since start-up and carries the saved frequency (applyConfig set it), so it is what the radio is told.
    req.keepCenterHz = carriedAirCentre();
    req.savedRadioRetry = true;
    launchDeviceOpen(std::move(req), restoreKeepLabel_);
}

void Engine::reopenAfterDriverFault() {
    // What pollSoapyRecovery decided is due. Everything below reads the dead
    // radio through device_ - the same object as soapyView_ whenever that
    // gate let it through - so the reopen itself is DeviceSource work and a
    // test can drive it with a device of its own (AppWindowTestAccess).
    if (device_ == nullptr) { return; }
    // THE DRIVER FAULTED AND THE RADIO IS BEING REOPENED BY ITSELF: counted, once
    // a session (at most once a minute, by itself, for as long as the driver keeps
    // faulting). Nothing of the fault - the driver's words, the radio - is kept.
    cascade::core::health::noteRecovered(cascade::core::health::Recovered::Reopen);

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
    // REMEMBERED HERE (engine/stage3b-pre fields-to-commands round 2): the
    // window used to write plutoUri_ in place, keystroke by keystroke; now it
    // edits a draft of its own and this is where the attempted address
    // becomes the persisted one - exactly what the direct write achieved,
    // just moved to the one moment that matters (an attempt), not every
    // keystroke. openPlutoFromBox() (the converter test's own call) already
    // built `args` FROM plutoUri_, so this is a harmless self-assignment
    // there.
    cascade::core::formatUtf8(plutoUri_, sizeof(plutoUri_), "%s", uri.c_str());
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

void Engine::openRtlTcpFromBox() { openRtlTcpAt(std::string("rtltcp=") + rtlTcpAddr_); }

void Engine::openRtlTcpAt(const std::string& args) {
    sourceError_.clear();
    // The Pluto's recipe, step for step (see openPlutoAt): the args name the address typed, the air frequency is read before the close, and
    // the open runs on the worker so a server that is not there spends its connect bound off the thread that must not wait.
    const std::string addr = (args.rfind("rtltcp=", 0) == 0) ? args.substr(7) : args;
    cascade::core::formatUtf8(rtlTcpAddr_, sizeof(rtlTcpAddr_), "%s", addr.c_str());
    const std::optional<double> keepCenterHz = carriedAirCentre();
    DeviceOpenResult req;
    if (device_ != nullptr) {
        req.closedRadio = cascade::gui::rememberedSourceAfterFailedOpen(
            sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, "",
            pipeline_.activeSource().sampleRateHz());
        req.closedLabel = deviceModel_;
        // The model, never the address: this line goes in the report.
        cascade::core::diagLogf("source: closing %s before opening the rtl_tcp server",
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
    req.kind = kRtlTcpDriverKey;
    req.args = std::string("rtltcp=") + addr;
    req.row = sourceSel_;
    req.requestRateHz = kSoapyRateHz[kSoapyRateDefaultIndex];
    req.keepCenterHz = keepCenterHz;
    launchDeviceOpen(std::move(req), "rtl_tcp server at " + addr);
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
    // THE USER CHOSE A SOURCE, so a saved radio that was only busy is no longer what the session is waiting for (0.99.92). restoreKeep_ stays
    // for the exit save; a failed pick must not be followed by a retry of the radio they left.
    cancelSavedRadioRetry();

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
            cascade::core::diagLogf("source: reopening the sound card (%s), which had stopped",
                                    cascade::source::loggableSoundCardDescription(soundCardLive_).c_str());
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
        if (kind == kPlutoDriverKey || kind == kRtlTcpDriverKey) {
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
    if (kind == "hydrasdr") { return std::make_unique<cascade::source::HydraSdrSource>(); }
    if (kind == "airspyhf") { return std::make_unique<cascade::source::AirspyHfSource>(); }
    if (kind == "sdrplay") { return std::make_unique<cascade::source::SdrPlaySource>(); }
    if (kind == "mirisdr") { return std::make_unique<cascade::source::MiriSdrSource>(); }
    if (kind == "rx888") { return std::make_unique<cascade::source::Rx888Source>(); }
    if (kind == "aor") { return std::make_unique<cascade::source::AorSource>(); }
    if (kind == kPlutoDriverKey) { return std::make_unique<cascade::source::PlutoSource>(); }
    if (kind == kRtlTcpDriverKey) { return std::make_unique<cascade::source::RtlTcpSource>(); }
    if (kind == "soapy") { return std::make_unique<cascade::source::SoapySource>(); }
    return nullptr;
}

void Engine::scanNative() {
    // SAID, once per walk, so a log shows when the native radios were listed -
    // and tests/test_main_view that showing the patch view lists none.
    cascade::core::diagLogf("source: listing native radios");
    // NO GATE, and that is the whole point of having our own transport. All
    // seven USB enumerations read SetupAPI device properties and
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
    if (testHooks_.nativeScanDetail != nullptr) {
        // The test's whole walk (API 0.3 8.10): the rows AND what a real walk leaves beside them - the unbound
        // dongles, the SDRplay API's advice and where it looked, whether an API row was found - so the sentences
        // the Source section builds from them are reachable with no USB walk and no SDRplay install. No Pluto row
        // is added: the test lists one when it wants one.
        NativeScanResult walk;
        testHooks_.nativeScanDetail(walk);
        nativeDevices_ = std::move(walk.devices);
        nativeRowLabels_.clear();
        for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
            nativeRowLabels_.push_back(d.label);
        }
        nativeUnbound_ = std::move(walk.unbound);
        sdrPlayAdvice_ = std::move(walk.sdrPlayAdvice);
        sdrPlayApiDetail_ = std::move(walk.sdrPlayApiDetail);
        sdrPlayRowsFound_ = walk.sdrPlayRowsFound;
        followSourceRowAfterRescan(rowsBefore);
        return;
    }
    if (testHooks_.nativeScan != nullptr) {
        // The test's list stands in for the USB walk (see testHooks_): no
        // enumeration of any kind runs, and nothing is reported unbound. The
        // selection is followed exactly as after a real scan, so a test can
        // move rows under it.
        nativeDevices_ = testHooks_.nativeScan();
        // Published like a real scan's, so a test that drives the Soapy
        // Mirics rule through the window sees the rows it listed.
        cascade::source::sdrPlayPublishNativeRows(nativeDevices_);
        nativeRowLabels_.clear();
        for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
            nativeRowLabels_.push_back(d.label);
        }
        nativeUnbound_.clear();
        dropRefusedMiricsSoapyRows();
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
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateHydraSdr()) {
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
    // AOR DIGITAL-I/Q INTERFACES (08D0:A001). Listed from the bus alone, like
    // every row above: the control serial port is paired - by asking the
    // receiver VR - only when the row is OPENED, never here (rule 1, and a
    // scan that wrote to serial ports would be writing to whatever else is on
    // an FTDI cable).
    for (cascade::source::NativeDeviceInfo& d : cascade::source::enumerateAor()) {
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

    // ONE RADIO, ONE ROW (0.99.9, at the owner's word; revised 2026-09-28
    // twice). An RSP reachable both natively and through the SDRplay API was
    // offered TWICE, under two names, with nothing saying which to pick - and
    // picking the native one gets the less capable of the two at best, and
    // (the two RSP1A field reports on 0.99.42) a guaranteed-failing open at
    // worst, once the API is installed - ON WINDOWS. source/rsp_rows.hpp
    // holds the rule and the platform switch (kNativeMiricsOpenUnsafeWithApi):
    // on Windows, hidden whenever the API is present on this machine, full
    // stop, regardless of whether THIS scan's enumeration listed anything -
    // a service that is merely slow to answer looks identical to one that is
    // down, and the row must not flicker back into a state that can only
    // fail. On Linux, the original narrower rule (hidden only when the API
    // actually lists the same radio) still applies - usbfs does not enforce
    // the Windows-specific reason native access is unsafe there, so a Linux
    // user's native row may genuinely work while the daemon is stopped.
    // ONE CHIP, ONE ROUTE: the list AS IT STANDS BEFORE THE DUPLICATES GO is
    // the only record of which radios the SDRplay API manages, and a SoapySDR
    // open is made on a worker that cannot see this window's state. Handed to
    // source/ here so SoapySource::open can ask the same question the hiding
    // below answers (see source/rsp_rows.hpp).
    cascade::source::sdrPlayPublishNativeRows(nativeDevices_);
    const bool sdrPlayApiInstalled = cascade::source::sdrPlayApiPresent();
    const bool sdrPlayOrphaned = cascade::source::anyRspOrphanedByHiding(
        nativeDevices_, sdrPlayApiInstalled, cascade::source::kNativeMiricsOpenUnsafeWithApi);
    {
        const std::size_t before = nativeDevices_.size();
        nativeDevices_ = cascade::source::withoutDuplicateRsps(
            nativeDevices_, sdrPlayApiInstalled, cascade::source::kNativeMiricsOpenUnsafeWithApi);
        const std::size_t hidden = before - nativeDevices_.size();
        if (hidden > 0) {
            // LOGGED, because a row that vanishes without explanation is the
            // fault report this is trying to prevent, arriving from the other
            // direction.
            cascade::core::diagLogf(
                "source: %zu native SDRplay row(s) hidden - the SDRplay API is installed%s",
                hidden,
                sdrPlayOrphaned ? " but did not list at least one of them this scan" : "");
        }
    }
    // ...AND WHAT TO SAY WHERE A HIDDEN ROW WOULD HAVE BEEN, OR WHEN THERE IS
    // NO RSP ROW BECAUSE THERE IS NO API. Composed here rather than in the
    // draw, because the draw runs sixty times a second and this reads the
    // process's load result.
    //
    // THE PRIORITY ORDER IS sdrPlaySourceAdvice()'s, not repeated here: a lost
    // session, a held-off enumeration, or an API too old always wins; "did not
    // list this radio" only wins when the API answered cleanly and simply does
    // not have this exact radio. The sentence comes from the driver, which is
    // pure and pinned by a test: it is the only instruction an RSP owner gets.
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
    const float sdrVersion = cascade::source::sdrPlayApiVersion(sdrApi);
    sdrPlayAdvice_ = cascade::source::sdrPlaySourceAdvice(
        sdrPlayOrphaned, sdrPlayRowsFound_, sdrApi.resolved, sdrVersion,
        cascade::source::sdrPlayLastEnumerationSkip());
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
    // THE RTL_TCP SERVER, THE SAME KIND OF ROW AND FOR THE SAME REASON: a
    // server on a network cannot be found by looking, so one fixed row follows
    // the Pluto's, and choosing it shows an address box and an Open key.
    {
        cascade::source::NativeDeviceInfo rtlTcp;
        rtlTcp.driver = kRtlTcpDriverKey;
        rtlTcp.label = FOX_TR_NOOP("rtl_tcp server (network)");
        // Kept in step with the box, as the Pluto's args are, so a restored
        // server's saved nativeArgs matches this row (see applyConfig).
        rtlTcp.args = std::string("rtltcp=") + rtlTcpAddr_;
        nativeDevices_.push_back(std::move(rtlTcp));
    }
    nativeRowLabels_.clear();
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        // "(native)" is not decoration: with a Soapy row for the same dongle
        // two lines below it, the user has to be able to see which one they
        // are picking, and which one they got. The Pluto's row is exempt: it
        // is not a radio that was found, and "(native)" on it would suggest
        // there is a non-native row for the same board somewhere.
        if (d.driver == kPlutoDriverKey || d.driver == kRtlTcpDriverKey) {
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
    for (const cascade::usb::UsbId& id : cascade::source::hydraSdrUsbIds()) {
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
    // An AOR I/Q interface on Windows arrives bound to AOR's own AorAlpha.sys,
    // which is exactly "present and unreachable" - see the sentence below.
    for (const cascade::usb::UsbId& id : cascade::source::aorUsbIds()) {
        ids.push_back(id);
    }
    nativeUnbound_ = cascade::usb::enumerateUnbound(ids);

    // The API's state or the managed radios may have changed since the last
    // Soapy scan: a lost session, or an RSP the API now lists. The selection
    // is followed by what it is, below, so a row that goes does not move it.
    dropRefusedMiricsSoapyRows();
    followSourceRowAfterRescan(rowsBefore);
}

void Engine::dropRefusedMiricsSoapyRows() {
    // Only a list that holds a Mirics-family row needs the API's state, and
    // asking it loads the vendor library: a machine with no such row never does.
    bool any = false;
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        if (cascade::source::isMiricsSoapyArgs(d.args)) {
            any = true;
            break;
        }
    }
    if (!any) { return; }
    const std::size_t before = soapyDevices_.size();
    soapyDevices_ = cascade::source::withoutRefusedMiricsSoapyRows(
        soapyDevices_, cascade::source::sdrPlayApiState(),
        cascade::source::sdrPlayPublishedNativeRows(), cascade::source::kNativeMiricsOpenUnsafeWithApi,
        soapyView_ != nullptr ? deviceArgs_ : std::string());
    const std::size_t hidden = before - soapyDevices_.size();
    if (hidden > 0) {
        // LOGGED, like the native rows hidden beside it: a row that vanishes
        // without explanation is the report this prevents from the other side.
        cascade::core::diagLogf(
            "source: %zu SoapySDR SDRplay/Mirics row(s) hidden - another route to the same chip "
            "owns it, or the SDRplay API session was lost",
            hidden);
    }
}

std::string Engine::sourceIdForRow(int row) const {
    if (row == 0) { return "siggen"; }
    if (row == 1) { return "row:iqfile"; }
    if (row == kSoundCardRow) { return "row:soundcard"; }
    const int n = row - kNativeRowBase;
    if (n >= 0 && n < static_cast<int>(nativeDevices_.size()) && row < soapyRowBase()) {
        const cascade::source::NativeDeviceInfo& d = nativeDevices_[static_cast<std::size_t>(n)];
        return d.driver + ":" + d.args;
    }
    const int sIdx = row - soapyRowBase();
    if (sIdx >= 0 && sIdx < static_cast<int>(soapyDevices_.size())) {
        return "soapy:" + soapyDevices_[static_cast<std::size_t>(sIdx)].args;
    }
    return {};
}

void Engine::takeScanDiagnostics(bool wholeAndEmpty) {
    ScanDiagnostics d;
    bool asked = false;
    if (testHooks_.scanDiagnostics != nullptr) {
        d = testHooks_.scanDiagnostics();
        asked = true;
    } else if (testHooks_.soapyScan == nullptr) {
        // The real answers. Not with soapyScan hooked: that hook means "no SoapySDR in this process", and the
        // first of these calls loads the runtime and edits the process's environment.
        for (const cascade::source::FaultedDriver& f : cascade::source::sessionFaultedDrivers()) {
            d.faultedDrivers.push_back(f.driver);
        }
        if (wholeAndEmpty) {
            d.modules = cascade::source::SoapySource::loadedModules();
            d.vendors = cascade::source::SoapySource::vendorInstalls();
            d.searchPaths = cascade::source::SoapySource::moduleSearchPaths();
        }
        asked = true;
    }
    faultedDrivers_ = asked ? std::move(d.faultedDrivers) : std::vector<std::string>();
    ScanDiagnostics next;
    if (asked && wholeAndEmpty) {
        next.modules = std::move(d.modules);
        next.searchPaths = std::move(d.searchPaths);
        next.vendors = std::move(d.vendors);
    }
    const auto sameVendors = [](const std::vector<cascade::source::VendorRoot>& a,
                                const std::vector<cascade::source::VendorRoot>& b) {
        if (a.size() != b.size()) { return false; }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].name != b[i].name || a[i].root != b[i].root) { return false; }
        }
        return true;
    };
    if (next.modules != scanEvidence_.modules || next.searchPaths != scanEvidence_.searchPaths ||
        !sameVendors(next.vendors, scanEvidence_.vendors)) {
        scanEvidence_ = std::move(next);
        ++scanEvidenceSeq_;
    }
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

// The label of the scanned row this radio was opened from, or - when that row
// is not in the current list (an rtl_tcp or Pluto address typed since the scan,
// a radio restored before the scan ran) - the family's plain name. NEVER the
// args: they are a serial or an address, and until 0.99.88 this fallback put
// them into deviceModel_, and from there into the usage report, the log and
// the crash context ("serial=170204A600", "rtltcp=garage.lan:1234").
std::string Engine::nativeLabelFor(const std::string& kind, const std::string& args) const {
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        if (d.driver == kind && d.args == args) { return d.label; }
    }
    return cascade::core::nativeFamilyModel(kind);
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
        // ...AND THE RTL_TCP ROW LIKEWISE: one row, whatever address is typed.
        const bool addressRow = d.driver == kPlutoDriverKey || d.driver == kRtlTcpDriverKey;
        keys.push_back({d.driver, addressRow ? std::string() : d.args});
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        keys.push_back({"soapy", d.args});
    }
    return keys;
}

void Engine::adoptDeviceMirrors(cascade::source::DeviceSource& dev, const std::string& kind,
                                   const std::string& args, double requestRateHz,
                                   bool restoreRemembered) {
    // ONE COPY OF THIS, shared by the worker-thread open and the synchronous
    // config restore. There were two before, and they had already drifted:
    // only one of them pointed the Rate combo at the device's ACTUAL readback
    // rather than at what was asked for.

    // THE RATE MENU IS THE DEVICE'S OWN LIST NOW. It was the fixed 1/2/4/8
    // MS/s table for every radio on every driver - which an RTL-SDR cannot
    // actually do two of (it has no 4 MS/s and no 8) and which stops 2.4 MS/s,
    // the rate ADS-B needs, from ever appearing.
    // (An Airspy's own memory - decimation, gain mode, gains - is already on
    // the radio by now: both open paths put it there straight after open()
    // and before the rate, because it decides which rates the request is
    // matched against. See DeviceOpenResult::airspyAtOpen.)
    deviceRatesHz_ = dev.supportedSampleRatesHz();
    deviceRateLabels_.clear();
    const bool airspyRates = cascade::gui::asAirspy(&dev) != nullptr;
    for (const double r : deviceRatesHz_) {
        deviceRateLabels_.push_back(airspyRates ? cascade::gui::airspyRateLabel(r) : rateLabel(r));
    }
    const double actualHz = dev.sampleRateHz();
    deviceRateIndex_ = nearestIndex(deviceRatesHz_, actualHz > 0.0 ? actualHz : requestRateHz);

    // AGC probe doubling as initialization: explicitly select manual gain
    // mode (matching the unchecked box). A device that says it has no gain
    // mode gets the documented "grey the checkbox" answer, not an error.
    //
    // NOT FOR AN AIRSPY: its two AGC switches belong to its gain mode, which
    // was put back just above, and switching them off here would undo a
    // remembered Free mode with its AGCs on. The mirror reads what it is.
    if (cascade::gui::asAirspy(&dev) != nullptr) {
        deviceAgcSupported_ = true;
        deviceAgc_ = dev.autoGain();
    } else {
        deviceAgcSupported_ = dev.autoGainSupported() && dev.setAutoGain(false);
        deviceAgc_ = false;
    }

    // THE GAINS, AND THEIR REAL RANGES. The sliders were 0..60 dB for every
    // stage of every radio; they are now what the driver says it will accept.
    deviceGainRanges_ = dev.gains();
    deviceGainNames_.clear();
    deviceGainsDb_.clear();
    manualGainsBeforeAgc_.clear();  // another radio's numbers are not this one's
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

    // THE MANUAL GAIN THE USER LEFT THIS RADIO AT (0.99.75): the 30 dB pushed into every
    // stage of a Soapy radio just above is a boot value, and the owner runs his B200 at
    // 76 dB - every start began 46 dB down. Whatever stage the user last set on THIS radio
    // (the same key as the switch below: driver and serial, the whole args for a radio with
    // no serial) is put back here: only for a stage the radio still reports, clamped into
    // that stage's own range, and the mirror is read back so the dial and the sliders show
    // what the radio TOOK. After the push and the first readback, because it overrides the
    // one and is read in place of the other; BEFORE the remembered auto gain below, so a
    // radio that comes back on auto gain has the user's numbers under it, to return to
    // when the switch goes off.
    //
    // ONLY ON AN ORDINARY OPEN (restoreRemembered): a recovery reopen after a driver fault
    // gets the gains the user had a moment ago from its caller. NOT AN AIRSPY: its gains are
    // part of its gain mode, which airspyMemory_ already puts back. A refused stage is
    // skipped without a word - it is the radio's to turn down - and one line says what went
    // back, so a gain that came back different from the dial's last position can be traced.
    if (restoreRemembered && cascade::gui::asAirspy(&dev) == nullptr) {
        restoreManualGain(dev, args);
    }

    // THE AUTO GAIN THE USER LEFT THIS RADIO ON (0.99.73): the probe above has
    // put every radio but an Airspy on manual gain, and the sliders have read
    // the manual values off it - so a radio whose box was ticked when it was
    // last used is put back on auto gain HERE, after both, and the box and the
    // greyed sliders then say so (deviceAgc_). Per radio, under the same key as
    // the bias tee (driver and serial; the whole args for a radio with no
    // serial). A "false" or no entry leaves the manual gain the probe chose,
    // which is what every open did before this. Nothing is erased on an open: the
    // map keeps the radios that are not plugged in.
    //
    // NOTHING HERE IS DRIVER-SPECIFIC: every native driver, an rtl_tcp server
    // and a Soapy device that answers autoGainSupported() gets it through the
    // same setAutoGain call the box makes. A radio that does not support auto
    // gain, or refuses the call, stays on manual gain with the box off - said
    // once in the log, since a user who ticked it last time would otherwise find
    // it unticked with no reason given.
    //
    // NOT AN AIRSPY: its two AGC switches are part of its gain mode, which the
    // Airspy path put back at open (AppConfig::airspy), and the code above read
    // them back into deviceAgc_. And not a recovery reopen (restoreRemembered
    // false): the caller applies the state the user had a moment ago.
    if (restoreRemembered && cascade::gui::asAirspy(&dev) == nullptr) {
        const auto mem = autoGainMemory_.find(cascade::core::biasTeeRadioKey(dev.driverKey(), args));
        if (mem != autoGainMemory_.end() && mem->second) {
            if (deviceAgcSupported_ && dev.setAutoGain(true)) {
                deviceAgc_ = true;
            } else {
                cascade::core::diagWarnf("source: auto gain remembered on but the radio refused it");
            }
        }
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
    // AN AIRSPY's REMEMBERED STATE before the rate, as the worker open does
    // (see DeviceOpenResult::airspyAtOpen for why the order matters).
    if (cascade::source::AirspySource* air = cascade::gui::asAirspy(dev.get())) {
        cascade::gui::airspyApplyRemembered(airspyMemory_, args, *air);
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
    // THE REPORT CONTEXT SAYS THE RATE, so it is rendered on every way out of
    // here - a rate change by the Rate combo, a plugin preset or the browser, and
    // every source change, all end in this function - and it is the rate the
    // source reads back, not the one that was asked for. A 30.72 MS/s Pluto was
    // the whole cause of one 0.99.59 crash and was nowhere in its context.
    DiagContextOnExit refreshOnExit{*this};
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

bool Engine::detachAndUnloadPlugins(cascade::core::PhaseClock* phases) {
    // A plugin reload - and a removal's unload - is the scope `plugins-reload` wherever it is reached from (0.99.64, core/frame_timing.hpp).
    cascade::core::OwnedFrameScope reloadScope(cascade::core::FrameScope::PluginsReload);
    // WHERE THE TIME GOES, for the one caller that asks (rescanPlugins): each of the four steps below is a different kind of wait with a
    // different owner - a decoder's destroy(), the patch page's decoders and the radios' readers, the panels' and the map's and the track
    // lookup's destroy() (a basemap that joins its tile workers), and the loader unmapping the modules - and a log line that says only
    // "the rescan took two minutes" cannot tell them apart.
    //
    // EVERY CALLER'S STEPS ARE PUBLISHED, even the ones that ask for no timing (0.99.84): a removal's unload, a stop and the exit path run
    // these same four waits, so a freeze there must name its step in the watchdog's report too (`reload-phase:`, core/hang_watchdog.cpp).
    // The caller's clock already publishes; the local one only publishes, and its laps are never logged - the exit path must not write a
    // "reload" line at every quit (tests/test_plugin_rescan_log.cpp). Built only when there is no caller's clock, because a publishing clock
    // clears the published name when it dies, and this one dies while the caller's still has its lap open.
    std::optional<cascade::core::PhaseClock> publishOnly;
    if (phases == nullptr) { publishOnly.emplace(cascade::core::PhaseClock::Publish{}); }
    cascade::core::PhaseClock* const clock = (phases != nullptr) ? phases : &*publishOnly;
    const auto lap = [clock](const char* name) { clock->begin(name); };

    // NOTHING IS LOADED AFTER THIS, so no signature taken for an earlier scan describes what is loaded (rescanPluginsIfChanged): a removal
    // calls this and then rescans, and the rescan is what makes the signature true again.
    pluginScanSigValid_ = false;

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
    lap(cascade::core::kReloadPhaseDecoders);
    // A PLUGIN BEING UNLOADED ALWAYS UNKEYS (stub campaign B13; a hardening master lacked): the key goes up and the window closes before the decoders - and the transmit
    // instances with them - are destroyed.
    txPluginModuleGoing(std::string(), "the plugins were unloaded");
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
    lap(cascade::core::kReloadPhasePatch);
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
    // a module whose destroy() is code in that same module - and so are the basemap's handle and tile borrows and the
    // track-info client's. All of them are the INTERFACE'S, held on its thread and polled from its frame, so this does not
    // reach for them (it used to, through two window hooks and PluginUi::clear(), which was safe only while the frame was the
    // pass): it posts a RELEASE directive - "let go of everything borrowed from the modules" - and unmaps only once the
    // interface has said it did. THE HANDSHAKE (pump move step P3, owner decision 2): where the calling thread is the
    // interface's own, or no control thread runs, the release is applied right here; where it is not, the wait is bounded
    // (kPluginUiHandshake), the control thread's watchdog is paused across it, it ends early when the control thread is
    // stopping, and a wait that runs out means DO NOT UNLOAD. Unmapping a module the interface still holds is the failure this
    // exists to prevent; an unload that cannot happen yet is retried (deferPluginUnload).
    lap(cascade::core::kReloadPhasePanels);
    {
        cascade::core::PluginDirective release;
        release.release = true;
        release.stopped = stoppedPluginIds();
        if (!deliverPluginDirective(std::move(release))) {
            cascade::core::diagWarnf(
                "plugins: the interface did not release the plugin UI (it is not framing; waited at most %d ms); the modules are NOT unloaded, and this will be retried",
                static_cast<int>(kPluginUiHandshake.count()));
            return false;
        }
    }
    // The API half that follows the instances (PluginUi::clear() did these beside destroying them): no module is live until
    // the next rebuild attaches it, and the grants and the request_tune records go, to be re-applied or re-learned. AFTER the
    // release, so a plugin saving its settings from destroy() still found itself live.
    pluginApi_->setLiveSet({});
    pluginApi_->clearGrants();
    pluginApi_->clearTuneRecords();

    if (testHooks_.pluginsAboutToUnmap != nullptr) { testHooks_.pluginsAboutToUnmap(); }
    lap(cascade::core::kReloadPhaseUnload);
    pluginHost_.unloadAll();
    return true;
}

void Engine::setPluginUiApplier(PluginUiApplier applier) {
    std::lock_guard<std::mutex> lk(pluginApplierMutex_);
    if (applier) {
        pluginApplier_ = std::make_shared<const PluginUiApplier>(std::move(applier));
        pluginGuiThread_.store(std::this_thread::get_id());
        frontEnd_.store(true, std::memory_order_release);
    } else {
        pluginApplier_.reset();
        pluginGuiThread_.store(std::thread::id{});
        frontEnd_.store(false, std::memory_order_release);
    }
}

bool Engine::deliverPluginDirective(cascade::core::PluginDirective d) {
    std::shared_ptr<const PluginUiApplier> applier;
    {
        std::lock_guard<std::mutex> lk(pluginApplierMutex_);
        applier = pluginApplier_;
    }
    // Inline where nothing can be inside the plugin UI but this thread: no front end (the engine's own plugin UI, driven by
    // whichever thread posts), the front end's own thread, or no control thread (the pass IS the front end's frame).
    const bool inlineHere = applier == nullptr || std::this_thread::get_id() == pluginGuiThread_.load() ||
                            !controlThreadRunning();
    const std::uint64_t epoch = pluginLink_.post(std::move(d));
    if (inlineHere) {
        if (applier != nullptr) {
            pluginLink_.applyNewest(*applier);
        } else {
            pluginLink_.applyNewest([this](const cascade::core::PluginDirective& x) { headlessUi_.apply(x); });
        }
        return pluginLink_.appliedEpoch() >= epoch;
    }
    // THE SILENCE RULE. A wait that ran out recorded the interface's poll count; while that count has not moved the interface has
    // not framed since, and the pass does not wait on it again - a window that hangs costs the pass ONE bound (250 ms), not one
    // per refresh, which a transmitting pass cannot afford (its key is judged by this thread's heartbeat). The directive is
    // posted all the same and the interface applies it at its next frame.
    if (pluginUiSilentPolls_.load() == pluginLink_.polls()) {
        pluginUiWait_.silenceSkips.fetch_add(1);
        return false;
    }
    bool applied = false;
    const auto waitStart = std::chrono::steady_clock::now();
    {
        WaitWatchdogPause holdWatchdog(*this);
        applied = pluginLink_.waitApplied(epoch, kPluginUiHandshake, [this] { return !controlRun_.load(); });
    }
    // (A wait ended by the control thread stopping is not the interface's silence.)
    if (!applied && controlRun_.load()) { pluginUiSilentPolls_.store(pluginLink_.polls()); }
    {
        const std::uint64_t us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count());
        pluginUiWait_.waits.fetch_add(1);
        pluginUiWait_.totalUs.fetch_add(us);
        std::uint64_t prev = pluginUiWait_.maxUs.load();
        while (us > prev && !pluginUiWait_.maxUs.compare_exchange_weak(prev, us)) {}
        if (!applied && controlRun_.load()) { pluginUiWait_.timeouts.fetch_add(1); }
    }
    return applied;
}

void Engine::deferPluginUnload(PluginUnloadOp op, const std::string& file) {
    // A removal outranks a rescan (its retry rescans at the end anyway); nothing outranks a removal.
    if (pluginUnloadOp_ == PluginUnloadOp::Remove || pluginUnloadOp_ == PluginUnloadOp::RemoveBlocked) {
        if (op == PluginUnloadOp::Rescan) { return; }
    }
    pluginUnloadOp_ = op;
    pluginUnloadFile_ = file;
    pluginUnloadPolls_ = pluginLink_.polls();
}

void Engine::retryPluginUnload() {
    if (pluginUnloadOp_ == PluginUnloadOp::None) { return; }
    // Only once the interface has framed since the refusal: a wait on one that is still not framing would only stall the pass
    // for the bound again, every pass, which is the very thing the handshake must not turn into.
    if (pluginLink_.polls() == pluginUnloadPolls_) { return; }
    const PluginUnloadOp op = pluginUnloadOp_;
    const std::string file = pluginUnloadFile_;
    pluginUnloadOp_ = PluginUnloadOp::None;
    pluginUnloadFile_.clear();
    switch (op) {
        case PluginUnloadOp::Rescan: rescanPlugins(); break;
        case PluginUnloadOp::Remove: removeInstalledPlugin(file); break;
        case PluginUnloadOp::RemoveBlocked: removeBlockedPlugin(file); break;
        case PluginUnloadOp::None: break;
    }
}

const std::string& Engine::lastDeniedNow() {
    const std::uint64_t seq = pluginApi_->lastDeniedSeq();
    if (seq != apiLastDeniedSeq_) {
        apiLastDenied_ = pluginApi_->lastDeniedPlugin();
        apiLastDeniedSeq_ = seq;
    }
    return apiLastDenied_;
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
    WaitWatchdogPause holdWatchdog(*this);
    // THE WATCHDOG IS EXCUSED, THE FRAME IS NOT (0.99.64, stub campaign B18): the pause above is an application-paced one - code that is expected to finish - and a reload that holds
    // the window for seconds is exactly the freeze the slow-frame record is for, so it is its own scope and counts (core/frame_timing.hpp). Opened only where the calling thread is the
    // frame loop's or has been lent the timer for the wait that stands in for this call (OwnedFrameScope); the free-running product's control thread is neither.
    cascade::core::OwnedFrameScope reloadScope(cascade::core::FrameScope::PluginsReload);
    // AND TOLD TO THE SENTINEL (core/breadcrumb.hpp), the same way: a death inside a plugin's load or unload is the most likely death this
    // application has, and the report should say it was in one. Raised for the length of the call - which is also the first load.
    cascade::core::breadcrumb::ActivityScope sentinelPluginActivity(cascade::core::breadcrumb::kReloadingPlugins);
    ++pluginScans_;  // (API 0.3 8.8: hashed into the lists' fingerprint, so a rescan rebuilds the block)

    // WHERE THE TIME WENT, in one log line per rescan (0.99.63). A session of 0.99.58 logged the line that opens a rescan and, 119.976 s
    // later, the first of the plugins it had loaded, with nothing between them: six different kinds of wait happen in this function, in a
    // row, and the log could not say which of them had held it. Nothing in the source waits 120 s, so the two minutes belonged to something
    // this program was waiting FOR - a plugin's destroy() joining a worker, a disk, the loader - and which one is the whole of the fix. A
    // scope guard, because there is a `return` in the middle of this function: the rescan that returns early still says how long it took to
    // get there.
    //
    // This measures; it bounds nothing and excuses nothing. AND IT PUBLISHES (0.99.84): the line below is only written when the rescan
    // ENDS, and field report 7591C65EFB69DD94 was a rescan that did not end - thirty seconds of `plugins-reload` with the stalled stack
    // ending in system modules, and nothing anywhere saying WHICH of the steps' waits had held it. A publishing clock mirrors the open
    // step's name for the watchdog to report (`reload-phase:`).
    cascade::core::PhaseClock phases{cascade::core::PhaseClock::Publish{}};
    struct ReloadLog {
        cascade::core::PhaseClock& clock;
        ~ReloadLog() {
            clock.end();
            clock.log("plugins: reload", cascade::core::HangWatchdog::kDefaultThresholdMs / 1000.0);
        }
    } reloadLog{phases};

    // THE REPORT CONTEXT LISTS THE LOADED PLUGINS, and this function changes them on every path out of it - including the early return
    // below, taken after every plugin has been unloaded - so the block is rendered on the way out rather than at one of the exits (0.99.62).
    DiagContextOnExit refreshOnExit{*this};

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
    // (The belt is the window's, taken when it applies the release below - the engine used to call it through a hook,
    // beforePluginRescan, from this pass.)

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
    if (!detachAndUnloadPlugins(&phases)) {
        // The interface is not framing: nothing was unmapped, nothing below may run (a scan over mapped modules is exactly
        // what the order above exists to prevent). Retried when it frames.
        deferPluginUnload(PluginUnloadOp::Rescan, std::string());
        return;
    }
    pluginEnforceError_.clear();

    // Un-quarantine BEFORE taking the inventory. Reconciliation and
    // planUpdates both key off "is the file there", and a plugin that this
    // code renamed aside last frame would otherwise read as deleted by the
    // user — which planUpdates deliberately refuses to update, taking away the
    // one remedy a retired plugin has.
    //
    // THE DISK'S SHARE of the rescan starts here: the folder is listed, the quarantined files are renamed back, and every installed file is
    // re-hashed against the record made when it was installed (loadInventory) - all synchronous.
    phases.begin(cascade::core::kReloadPhaseInventory);
    std::string restoreError;
    if (!restoreQuarantinedPlugins(restoreError)) { pluginEnforceError_ = restoreError; }

    // THE MANIFEST AS THIS SCAN IS ABOUT TO READ IT, noted before it is read: the signature taken below, after the quarantine, must find it
    // exactly so. The catalogue worker writes the manifest from its own thread, and a floor written between the read and the listing would
    // be in the signature and not in the blocked set - a retired plugin the next automatic rescan would then decline to look for. Three
    // numbers from one stat; this is the slow path, which has just taken the folder apart.
    struct ManifestStamp {
        bool present = false;
        std::uint64_t bytes = 0;
        std::int64_t when = 0;
    };
    const auto stampOfManifest = [this]() {
        ManifestStamp s;
        const std::filesystem::path m(cascade::core::PluginRepo::manifestPath(pluginDir_));
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(m, ec);
        if (ec) { return s; }
        const std::filesystem::file_time_type when = std::filesystem::last_write_time(m, ec);
        if (ec) { return s; }
        s.present = true;
        s.bytes = static_cast<std::uint64_t>(bytes);
        s.when = static_cast<std::int64_t>(when.time_since_epoch().count());
        return s;
    };
    const ManifestStamp manifestRead = stampOfManifest();

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
    // PLUGINS REFUSED BEFORE THEY ARE LOADED are counted like those refused at load (0.99.64, core/health_events.hpp): retired by the
    // catalogue's floor, or built for another ABI - the reason class only, never which plugin. The ledger keeps one of each per session,
    // so every rescan may say it again.
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) {
        cascade::core::health::notePluginLoadRefused(b.reason == cascade::core::PluginBlockReason::AbiMismatch
                                                         ? cascade::core::health::LoadClass::Abi
                                                         : cascade::core::health::LoadClass::Retired);
    }

    // Judged afresh by this scan below; nothing is superseded by a scan that does not happen.
    pluginOldCopies_.clear();
    std::string quarantineError;
    if (!quarantineBlockedPlugins(quarantineError)) {
        // FAIL CLOSED. Scanning now would map the retired plugin along with
        // everything else, which is the precise state this feature exists to
        // prevent, so this run gets no plugins at all and says why.
        pluginEnforceError_ = quarantineError;
        return;
    }

    // LoadLibrary of every module, and each one's DllMain, and the descriptor query - the loader and whatever it has to read, off a disk that
    // may be cold or a scanner's.
    // THE SIGNATURE OF WHAT THE SCAN IS ABOUT TO LOAD (core/plugin_dir_signature.hpp), taken BEFORE it loads anything and AFTER the
    // quarantine, so it is the folder as the scan will see it - a retired module already renamed aside - and so equal to what the next check
    // finds if nobody touches the folder. A file that changes between this listing and the scan's own is then a difference the next check
    // sees, never one it misses. Kept only if the manifest in it is the one the inventory above read; judged at the end, with what the scan
    // loaded.
    cascade::core::PluginDirSignature scanSig;
    bool scanSigOk = (testHooks_.pluginDirSignature != nullptr ? testHooks_.pluginDirSignature
                                                               : &cascade::core::readPluginDirSignature)(pluginDir_, scanSig);
    if (scanSigOk) {
        const cascade::core::PluginDirSignature::Entry* m = scanSig.find(cascade::core::PluginRepo::manifestFileName());
        scanSigOk = (m != nullptr) == manifestRead.present &&
                    (m == nullptr || (m->bytes == manifestRead.bytes && m->writeTime == manifestRead.when));
    }

    phases.begin(cascade::core::kReloadPhaseLoad);
    // WHERE EACH FILE CAME FROM (0.99.74), from the install records the inventory above already read - the host reads no manifest of its
    // own. It is what lets the loader refuse to let a plugin site's module take a declared name from an official one
    // (resolveDuplicatePlugins).
    cascade::core::PluginFileOrigins origins;
    for (const cascade::core::InstalledPlugin& ip : pluginInventory_.plugins) {
        if (!ip.file.empty()) { origins[ip.file] = !ip.source.empty(); }
    }
    pluginHost_.scan(pluginDir_, origins);
    phases.begin(cascade::core::kReloadPhaseRestart);

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
    //
    // The old copies this scan found superseded - what the store's clean-up key offers, judged against this scan and no older one.
    pluginOldCopies_ = cascade::core::supersededPlugins(pluginHost_.plugins(), pluginInventory_.plugins);
    refreshPluginRunner();

    // THE SCAN RAN TO ITS END: the signature taken above is what is loaded, and rescanPluginsIfChanged may take it as that - unless
    // something this scan did is not explained by files. A module REFUSED (not turned off as an old copy of another) may have been refused
    // for what the loader found outside the folder - a runtime a module needs, since installed - which no listing of the folder can show,
    // so such a scan is never skipped afterwards. An enforcement error (a rename that failed, a restore that failed) is a scan that did not
    // finish what it set out to.
    bool refused = false;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (!p.loaded && p.supersededBy.empty()) { refused = true; }
    }
    pluginScanSig_ = std::move(scanSig);
    pluginScanSigValid_ = scanSigOk && pluginEnforceError_.empty() && !refused;
}

void Engine::rescanPluginsIfChanged() {
    // THE PAUSE THE RESCAN HOLDS, held across the listing that decides whether there is one: a folder that is slow to list is the case this
    // exists for, and the rescan it replaces would have listed it under this same pause and the same cap - a slow listing here is not a new
    // freeze to report.
    WaitWatchdogPause holdWatchdog(*this);
    // A listing that is slow is the same scope as the reload it would have been (see rescanPlugins).
    cascade::core::OwnedFrameScope reloadScope(cascade::core::FrameScope::PluginsReload);
    if (pluginScanSigValid_) {
        // PUBLISHED AS THE STEP `check` (0.99.84, core/plugin_reload_phases.hpp): a folder slow to list is the case this pause exists for,
        // and a freeze report taken while the listing hangs should say it was the listing - not leave the scope's `reload-phase: none`
        // pointing at the reload proper. The clock publishes only; there is no reload log line to lap.
        cascade::core::PhaseClock checking{cascade::core::PhaseClock::Publish{}};
        checking.begin(cascade::core::kReloadPhaseCheck);
        // The folder the scan would read now: the same call the rescan makes, so a folder that moved (the executable's own is no longer
        // writable, a package) is a different signature without further ado.
        const std::string dir = (testHooks_.pluginDir != nullptr) ? testHooks_.pluginDir() : cascade::core::PluginHost::defaultPluginDir();
        cascade::core::PluginDirSignature now;
        const bool listed = (testHooks_.pluginDirSignature != nullptr ? testHooks_.pluginDirSignature
                                                                      : &cascade::core::readPluginDirSignature)(dir, now);
        checking.end();
        if (listed && now == pluginScanSig_) {
            // Neither a name nor a path: which plugins there are is in the line each scan wrote when it loaded them.
            cascade::core::diagLogf("plugins: reload skipped - nothing changed since the last scan");
            return;
        }
    }
    rescanPlugins();
}

std::vector<cascade::core::PluginUpdate> Engine::plannedPluginUpdates() const {
    // Pure, and empty until a catalogue has been fetched this session:
    // catalog_ is only ever filled by the plugin store window - its first
    // open in a session, or CHECK NOW.
    return cascade::core::PluginRepo::planUpdates(catalog_, pluginInventory_.plugins);
}

std::vector<cascade::core::ScannedModule> Engine::scannedModules() const {
    std::vector<cascade::core::ScannedModule> out;
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        // A module refused before its descriptor could be read has no name and no version: there is nothing to say about it by version. (Its
        // install record, if it has one, still speaks - see PluginRepo::installStateFor.)
        if (lp.name.empty() || lp.version.empty()) { continue; }
        cascade::core::ScannedModule m;
        m.file = std::filesystem::path(lp.path).filename().string();
        m.version = lp.version;
        // The catalogue id comes from the install record that names this file; the scan itself only knows the module's declared name.
        m.id = lp.name;
        for (const cascade::core::InstalledPlugin& ip : pluginInventory_.plugins) {
            if (!ip.id.empty() && equalsFileNameAscii(ip.file, m.file)) {
                m.id = ip.id;
                // ...and the source it was installed from (0.99.74): the id alone does not name it.
                m.source = ip.source;
                break;
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

cascade::core::InstallState Engine::pluginInstallState(const cascade::core::PluginCatalogEntry& e) const {
    return cascade::core::PluginRepo::installStateFor(e, pluginInventory_.plugins, scannedModules());
}

std::string Engine::installedModuleFile(const cascade::core::PluginCatalogEntry& e) const {
    // The newest scanned copy of this id from this source: an update's "from" copy is the record to report, not nothing (the file the NEW
    // entry would write has never been on disk).
    std::string bestFile;
    std::string bestVersion;
    for (const cascade::core::ScannedModule& m : scannedModules()) {
        if (m.id != e.id || m.source != e.source) { continue; }
        if (bestFile.empty() || cascade::core::PluginRepo::compareVersions(m.version, bestVersion) > 0) {
            bestFile = m.file;
            bestVersion = m.version;
        }
    }
    if (!bestFile.empty()) { return bestFile; }
    const cascade::core::LoadedPlugin* lp = installedPluginRecord(e);
    return lp != nullptr ? std::filesystem::path(lp->path).filename().string() : std::string();
}

bool Engine::catalogEntryInstalled(const cascade::core::PluginCatalogEntry& e) const {
    // BY PLUGIN ID AND VERSION (0.99.72), from the install records and the scan - see the declaration.
    if (pluginInstallState(e).kind != cascade::core::InstallStateKind::NotInstalled) { return true; }
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
    // A picture on its way and a clean-up on a worker are transfers too (stub campaign B3): the same one slot.
    if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) {
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
    //
    // A PLUGIN FROM A SITE THE USER ADDED ALWAYS CARRIES A NOTICE (0.99.74): the page says it comes from somewhere else and that FoxSDR has
    // not reviewed it, whether or not its maker published a legal notice of their own, and the tick under that box is the consent. So the
    // gate asks for the acknowledgement of every such plugin - the same sentence, the same tick, the same web and ADD ALL paths a legal
    // notice always had.
    if ((!e.legalNotice.empty() || !e.source.empty()) && !acknowledged) {
        return cascade::gui::kLegalNoticeFirstReason;
    }
    return {};
}

void Engine::startAddAll(bool noticesAcknowledged, const std::string& acknowledgedId, bool updatesOnly) {
    if (addAllRun_.active || catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) { return; }
    // RE-PLANNED FROM LIVE STATE, not from the plan the key was drawn against.
    // The key was drawn one frame ago from a model built one frame ago, and
    // the whole point of this queue is that it acts over many frames.
    // PLANNED HERE (engine/stage3b-pre, docs/engine-stage3.md OPEN 5), from
    // the catalogue and the inventory, by the SAME rule the store window
    // letters its key from (engine/add_all_plan.hpp) - so a headless engine
    // runs ADD ALL too. The one thing only the window knew, which module's
    // own notice tick is on, comes in the command (acknowledgedId).
    const std::vector<cascade::core::PluginUpdate> updates = plannedPluginUpdates();
    std::vector<AddAllRow> rows;
    rows.reserve(catalog_.size());
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e = catalog_[static_cast<std::size_t>(i)];
        AddAllRow r;
        r.name = e.name;
        r.fitted = catalogEntryInstalled(e);
        const std::string key = cascade::core::PluginRepo::catalogueKey(e.source, e.id);
        for (const cascade::core::PluginUpdate& u : updates) {
            if (cascade::core::PluginRepo::catalogueKey(u.source, u.id) == key && !u.toVersion.empty()) { r.hasUpdate = true; }
        }
        // (A site's plugin always carries the notice the install gate asks about: see pluginInstallBlockedReason.)
        r.hasNotice = !e.legalNotice.empty() || !e.source.empty();
        r.blockedReason = pluginInstallBlockedReason(i, !acknowledgedId.empty() && key == acknowledgedId);
        r.blockedReasonIfAcknowledged = pluginInstallBlockedReason(i, true);
        rows.push_back(std::move(r));
    }
    AddAllCatalogue cat;
    cat.haveCatalogue = !catalog_.empty();
    cat.listedNothing = !catalogStatus_.empty();
    cat.lastCheckFailed = !catalogError_.empty();
    cat.busy = catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_;
    AddAllRule plan = planAddAllRows(rows, cat, noticesAcknowledged);
    if (updatesOnly) {
        // UPDATE ALL (0.99.72): the same queue with nothing to fetch in it. What is passed over for want of a build or a notice is not this
        // key's business, so it is not listed either; "nothing to update" is its own reason.
        plan.install.clear();
        plan.skipped.clear();
        if (cat.haveCatalogue && !cat.busy && plan.update.empty()) {
            plan.blockedReason = FOX_TR_NOOP("every module in the catalogue is already fitted, and none has a newer build");
        }
    }
    if (!plan.blockedReason.empty()) {
        installError_ = plan.blockedReason;
        return;
    }

    addAllRun_ = AddAllRun{};
    // BY CATALOGUE KEY (0.99.74): the id for an official plugin, "<id>@<site>" for a site's.
    for (const int i : plan.install) {
        const cascade::core::PluginCatalogEntry& pe = catalog_[static_cast<std::size_t>(i)];
        addAllRun_.ids.push_back(cascade::core::PluginRepo::catalogueKey(pe.source, pe.id));
        addAllRun_.isUpdate.push_back(false);
    }
    for (const int i : plan.update) {
        const cascade::core::PluginCatalogEntry& pe = catalog_[static_cast<std::size_t>(i)];
        addAllRun_.ids.push_back(cascade::core::PluginRepo::catalogueKey(pe.source, pe.id));
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
        plan.install.size(), plan.update.size(), plan.skipped.size());
    for (const std::string& s : plan.skipped) {
        cascade::core::diagLogf("plugin store: add all - passing over %s", s.c_str());
    }
}

void Engine::pumpAddAll() {
    if (!addAllRun_.active) { return; }
    // A transfer is in flight, or a catalogue fetch is: wait. Nothing here
    // cancels anything - CANCEL is the user's key and it stops the transfer,
    // which this queue then records as a failure and carries on past.
    //
    // (A clean-up on a worker - an update's own - and a picture are transfers too: a start would be refused, and a refused start is not a
    // failure to be stepped past. A picture cannot be on its way during a run: the queue waits behind it.)
    if (installPending_ || catalogPending_ || cleanupPending_ || screenshotPending_) { return; }

    while (addAllRun_.next < addAllRun_.ids.size()) {
        const std::string id = addAllRun_.ids[addAllRun_.next];
        const bool wantUpdate = addAllRun_.isUpdate[addAllRun_.next];
        ++addAllRun_.next;

        // THE ROW IS LOOKED UP AGAIN BY ID. catalog_ may have been replaced
        // since the queue was built, and an entry that is no longer there is a
        // failure with a reason rather than a silent skip.
        const int idx = catalogIndexByKey(id);
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
                if (cascade::core::PluginRepo::catalogueKey(u.source, u.id) != id) { continue; }
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
    // the run has taken them; the next press is a new decision. (The standard
    // window sees the run's entry leave STORE_ACTIVITY, or the summary arrive,
    // and clears the tick itself: AppWindow::storeLists.)
    host_->onAddAllFinished();
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
    if (!detachAndUnloadPlugins()) {
        deferPluginUnload(PluginUnloadOp::Remove, fileName);
        return;
    }
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
    //
    // AND THE DORMANT SET WITH IT (0.99.73, stub campaign B2): the plugins set to AUTO that nothing is using are given no instance by these
    // wholesale rebuilds either, so a new source or rate recreates only what is running. A plugin pinned to ALWAYS is running by
    // definition; it is marked so here, before the sets are worked out - and so is one that is in use for as long as it is fitted (a
    // processor in the audio chain), which this rebuild builds like an ALWAYS one, so its first lifecycle step has nothing to wake. A
    // WINDOW-LESS HOST (no front end installed) treats AUTO as pinned and marks every such plugin running too (engine_plugin_run.cpp).
    for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
        if (!lp.loaded) { continue; }
        const std::string lkey = cascade::core::pluginKey(lp);
        if (lkey.empty()) { continue; }
        const cascade::core::PluginRun mode = pluginRunMode(lkey);
        const std::string lid = cascade::core::pluginRunId(lkey);
        if (mode == cascade::core::PluginRun::Always ||
            (mode == cascade::core::PluginRun::Auto &&
             (cascade::core::pluginCapsStandingDuty(lp.capabilities) || !frontEndPresent()))) {
            pluginLife_[lid].running = true;
        } else if (mode == cascade::core::PluginRun::Stopped) {
            pluginLife_[lid].running = false;
        }
    }
    // The runner's stop and dormant sets and the API core's stop set (set at once, so a stopped plugin's marks and commands leave the
    // screen now and its queued requests are refused); the plugin UI's own copies ride in the directive below.
    pushPluginHoldSets();
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
    // THE CENTRE A PLUGIN READS IS THE PUBLISHED ONE (pump move step P3): this is called from the plugin UI's thread (a tracker's
    // poll) or a plugin's own worker, and the active source is swapped by the pass - a read of it from outside the pass is a read
    // of an object that can be replaced under it. The published state is the pass's own copy of the same figure, at most one pass
    // old. Before the first publish there is no pass beside this call (the scan in initialise), so the source is read directly.
    svc.centreHz = [this] {
        cascade::core::PublishedState ps;
        if (receiverSnapshot_->read(ps)) { return ps.rx.centreHz; }
        return controlThreadRunning() ? 0.0 : pipeline_.activeSource().centerFrequencyHz();
    };
    svc.sampleRateHz = [this] { return pipeline_.inputRateHz(); };
    svc.unixTimeMs = [] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    // THE LEGACY request_tune IS QUEUED, NOT APPLIED (pump move step P3, owner decision 1; plugin_abi.h's request_tune text).
    // PluginUi has already applied the per-plugin permission - stopped, requester recorded, granted - and this runs on
    // WHICHEVER THREAD THE PLUGIN CALLED FROM (the GUI thread inside a poll, or its own worker), so it must not touch the
    // receiver: it queues the retune on the API core, keyed on the module, and answers OK meaning ACCEPTED. The pass applies it
    // in applyPluginApi, at its next plugin phase, after checking the grant again. What it can still tell the plugin at the
    // call is "no device": a source with no usable rate is one nothing has opened (there is no isOpen() on the IqSource
    // interface, and the signal generator has a rate) - read from the published state, the pass's own copy of the same figure,
    // not from the pipeline (a read of the source from outside the pass).
    svc.tuneFrom = [this](const std::string& key, double hz) -> std::int32_t {
        cascade::core::PublishedState ps;
        if (receiverSnapshot_->read(ps) && !(ps.rx.sampleRateHz > 0.0)) { return CASCADE_TUNE_NO_DEVICE; }
        return pluginApi_->queueCentreRetune(key, hz);
    };
    // THE GRANTS GO IN BEFORE THE DIRECTIVE: a plugin may ask for the receiver from inside its attach(), and must be answered
    // by the grant the user gave, not by a clear() the rescan did. They go straight to the core (thread-safe) and never
    // through the directive - a toggle must not rebuild a plugin's instances, and a grant applied from a directive posted a
    // moment ago could overwrite one the user has just changed.
    applyPluginTuneGrants();
    applyPluginSettingsGrants();
    applyPluginTransmitGrants();
    // THE BASEMAP, if a plugin supplies one. The first loaded plugin declaring
    // the capability wins: two basemaps cannot both be the map, and picking
    // silently by load order is more predictable than picking by some quality
    // the user cannot see. A rescan that removed the plugin takes its tiles - and its textures - with it (the release).
    //
    // A STOPPED plugin is skipped here too, and that is the whole of what
    // "stopped" means for a basemap: the map falls back to its built-in
    // coastlines rather than keeping tiles from a plugin the user has switched
    // off. Skipping it in the runner and the UI half but not here would leave
    // the stopped plugin drawing the entire map background.
    // (A DORMANT plugin is NOT skipped here: a basemap is a static table the map draws from, with no instance to create or poll, so being
    // dormant changes nothing about it - only STOPPED does. chooseBasemapAndTrackInfo is the one choice, shared with the life-only directive.)
    // Track enrichment, by the same first-wins rule and for the same reason.
    const CascadeBasemapApi* wantBasemap = nullptr;
    const CascadeTrackInfoApi* wantTrackInfo = nullptr;
    chooseBasemapAndTrackInfo(wantBasemap, wantTrackInfo);
    // THE DIRECTIVE: what the plugin UI is to be built over, as values. The interface rebuilds its track sources, panels and
    // instruments, attaches the host table (the ABI's attach(), before any capability's create()) and the basemap and the
    // track-info client from it, on its own thread. Where that thread is not this one the wait is bounded and is NOT an error
    // when it runs out - nothing is being unmapped - but it is why a decoder's create() below can overtake the attach() of a
    // plugin the interface has not yet been given, so it is said.
    {
        cascade::core::PluginDirective d;
        d.plugins = pluginHost_.plugins();
        d.stopped = stoppedPluginIds();
        d.dormant = pluginDormantIds_;
        d.services = std::move(svc);
        d.basemap = wantBasemap;
        d.trackInfo = wantTrackInfo;
        if (!deliverPluginDirective(std::move(d))) {
            cascade::core::diagWarnf(
                "plugins: the interface had not applied the plugin directive (it is not framing; waited at most %d ms); the decoders are built without it",
                static_cast<int>(kPluginUiHandshake.count()));
        }
    }
    // The receiver's stream clock, which the runner writes and the
    // level-1 get_stream_info reads (CascadeStreamInfo).
    pluginRunner_.setStreamClock(pluginApi_->streamClock());
    // A REBUILD DESTROYS THE TRANSMIT INSTANCES: a plugin that is keyed loses its key and its window here (stub campaign B13), not a block later; one that is merely armed
    // is left as it is (its instances come back asking, and the window is still the operator's).
    txPluginModuleGoing(std::string(), "the plugins were rebuilt");
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

    // (The demonstration instruments' windows, the basemap and the track-info client are the interface's, chosen from the
    // directive above and applied on its thread: the engine used to reach them through three hooks - showDemonstrationInstrument,
    // attachBasemap and attachTrackInfo - from this pass. The grants are not re-applied here: they are the core's, set before the
    // directive, and nothing between clears them.)
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
    // The typed fields, every map page's home, the scope and the coverage map follow in the WINDOW, which looks at the
    // position the state carries once a frame (AppWindow::followReceiverPosition; window port group 14, part a) - the
    // engine no longer calls it from its pass.
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
        // (The rail's "Receiver position" fold opens when the fix lands: the window sees the GPS entry go to Fixed and
        // does that itself - AppWindow::readGpsRow, window port group 14 part b. The engine no longer calls it.)
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
        // A PLUGIN THE USER HAS STOPPED IS STILL A PART (owner, 2026-10-26:
        // "make so the plugin doesn't have to be on to get the components").
        // Until then a stop took the plugin out of the patch as well, so the
        // parts bin and the blueprints list offered nothing for it until it
        // was switched on in DECODE first. A patch decoder is built and fed
        // by the patch's own runner (patch_runner.hpp), never by
        // pluginRunner_, so a stop now governs only the receiver's copy of
        // the plugin, and a patch node that names it runs either way.
        // (Master's 0.99.94, 5584212c, applied to the Engine, which holds the catalogue since stage 4.)
        const std::string key = cascade::core::pluginKey(p);
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
    return cascade::core::patch::decoderIsShown(patchGraph_, node);
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
                                  const CascadePreset& ps, bool pin) {
    // A PRESET ON A STOPPED PLUGIN STARTS IT. Pressing "ADS-B 1090 MHz" is an
    // unambiguous "I want this plugin now", and the alternative is the worst
    // outcome this feature can produce: the radio dutifully retunes to 1090 MHz
    // in the mode the plugin asked for, and then nothing decodes, because the
    // plugin the user just pressed a button on is switched off. Only recorded
    // here — the rebuild at the end of this function is the one that creates
    // the instances, and it has to happen after the receiver has moved anyway.
    //
    // The row keeps its Stop button, so this is undone with one click.
    //
    // AND IT PINS THE PLUGIN TO ALWAYS (0.99.73): a preset press is the deliberate "run this", so the plugin goes on running when its
    // window is closed. The tune that merely OPENING a window does (pin == false) leaves the run state alone - the window wants the
    // plugin while it is open, and that is all it should do.
    if (pin) { recordPluginStopped(cascade::core::pluginKey(p), false); }

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
            commitModeIndex(idx);
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
            cascade::gui::applySourceRate(*device_, ps.sampleRateHz, sourceError_, /*explicitRequest=*/true);
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
        const double bwBefore = vfoBandwidthHz_;
        vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(ps.bandwidthHz, bwHi));
        pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
        // -1 WHEN THE PRESET ASKED FOR SOMETHING THE LIST DOES NOT CARRY, and
        // that is exactly the APT case: 40 kHz is not a step, and the nearest
        // one is 12.5 kHz. Pointing the combo there made it letter a bandwidth
        // the receiver was not running and, on the next click, apply it.
        bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
        if (vfoBandwidthHz_ != bwBefore) { logBandwidthChange("decoder preset"); }
    }

    // FLAT AUDIO (CASCADE_PRESET_FLAT_AUDIO, stub campaign B5 - master's, lost with the rest of applyPluginPreset's 0.99.7x lines): the receiver's
    // de-emphasis goes Off, the state saved and shown by the receiver control.
    if ((ps.flags & CASCADE_PRESET_FLAT_AUDIO) != 0u) {
        deemphIndex_ = 2;
        pipeline_.setDeemphasisUs(kDeemphUs[deemphIndex_]);
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
    // (The windows a preset opens are the interface's to open, on its own thread: it is TOLD, by an event - API 0.3 8.14,
    // FOXAPI_EVENT_PRESET_APPLIED, text the module's file name - raised once per application, in this pass's endPass. The
    // engine used to call the window's openPluginWindowsFor from here, which writes the map pages, the plugin windows and the
    // Decoder output window's flag.)
    api_->presetApplied(cascade::core::pluginKey(p), ps.flags);

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
    for (const std::string& k : pluginTuneAllowed_) { pluginApi_->setTuneGranted(k, true); }
}

void Engine::setPluginTuneAllowed(const std::string& pluginKey, bool allowed) {
    // (An empty key never holds a grant - the core's own rule, as PluginUi's was.)
    if (!pluginKey.empty()) { pluginApi_->setTuneGranted(pluginKey, allowed); }
    const auto it =
        std::find(pluginTuneAllowed_.begin(), pluginTuneAllowed_.end(), pluginKey);
    if (allowed && it == pluginTuneAllowed_.end()) {
        pluginTuneAllowed_.push_back(pluginKey);
    } else if (!allowed && it != pluginTuneAllowed_.end()) {
        pluginTuneAllowed_.erase(it);
    }
}

void Engine::applyPluginSettingsGrants() {
    for (const std::string& k : pluginSettingsAllowed_) { pluginApi_->setSettingsGranted(k, true); }
}

void Engine::setPluginSettingsAllowed(const std::string& pluginKey, bool allowed) {
    // The live grant and the durable one in one place, exactly as the tune
    // grant does, so a grant that took effect can never fail to be saved.
    pluginApi_->setSettingsGranted(pluginKey, allowed);
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
    //
    // THE STATUS COLUMN'S "READY" (API 0.3 8.2, decodersMatched): the same
    // count with the receiver stopped too - instances built and matched to
    // the rate, nothing flowing - which the DECODERS card letters as "N ready".
    // That needs the runner's answer while stopped, so the one acquisition is
    // taken either way; with the receiver stopped there is no DSP thread
    // taking that lock per block to be held up by it. The modules' keys are
    // the cached ones (refreshApiPluginKeys hashes the plugins' paths and
    // names, and rebuilds the keys only when those moved): no key string is
    // built per plugin per pass.
    std::size_t runnerActive = 0;
    std::uint32_t matchedModules = 0;
    std::uint32_t fittedModules = 0;
    pluginRunner_.feedSnapshot(runnerActive, feedingKeysScratch_);
    refreshApiPluginKeys();
    {
        const auto& plugins = pluginHost_.plugins();
        for (std::size_t i = 0; i < plugins.size(); ++i) {
            const cascade::core::LoadedPlugin& p = plugins[i];
            if (!p.loaded) { continue; }
            if (p.decoder == nullptr && p.iqDecoder == nullptr && p.imageDecoder == nullptr) { continue; }
            ++fittedModules;
            const std::string& key = apiPluginKeys_[i];
            if (key.empty()) { continue; }
            if (std::find(feedingKeysScratch_.begin(), feedingKeysScratch_.end(), key) !=
                feedingKeysScratch_.end()) {
                ++matchedModules;
            }
        }
    }
    // RUNNING is fed with the receiver running; stopped, nothing is fed.
    const std::uint32_t fedModules = running ? matchedModules : 0u;

    // THE RECEIVER RAIL'S READOUTS (API 0.3 8.4), each read ONCE: the flag word,
    // the members below and /api/status's extension (AppStateExt) all take
    // these. (test.autoNotch and test.pilot stand in for the DSP thread's
    // readout in a hermetic engine, which hears no tone and has no pilot.)
    const bool autoNotchEngaged = testAutoNotchOn_ ? testAutoNotchEngaged_ : pipeline_.autoNotchEngaged();
    const double autoNotchHz = testAutoNotchOn_ ? testAutoNotchHz_ : pipeline_.autoNotchFrequencyHz();
    const bool pilotLocked = testPilotOn_ ? testPilotLocked_ : pipeline_.pilotLocked();

    // THE FLAGS: each condition, from its own source, into its own bit
    // (core::receiverFlags; test_snapshot_app walks them one at a time).
    cascade::core::RxFlagSources fs;
    fs.running = running;
    fs.deviceOpen = device_ != nullptr;
    fs.faulted = pipeline_.faulted() || !testFault_.empty();
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
    // THE KEY, as the engine API's rules decided it this pass (stage 4).
    const ApiHost::TxFacts key = api_->txFacts();
    fs.txAvailable = transmitter_.haveSink();
    fs.txKeyed = transmitter_.transmitting();
    fs.txLatched = key.latched;
    // The local operator's consent to a remote key: TX_REMOTE_ARM from a
    // LOCAL session (the Transmit page's "Allow remote transmit"); what
    // /api/status calls transmitAvailable, with a transmitter open.
    fs.txRemoteArmed = key.remoteArmed && transmitter_.haveSink();
    fs.sinkOpen = sink.running();
    // API 0.3: the bias tee's key (the radio's readback), and the patch.
    fs.biasTeePresent = device_ != nullptr && biasTeePanel_.present;
    // THE KEY'S LAMP, whatever it is drawn over (8.3): the stand-in's state
    // while the test stand-in is in front of the key (it is only ever active
    // where no real radio with a bias tee is reachable), else the radio's own
    // readback. The window draws the lamp from this flag and knows no stand-in.
    fs.biasTeeOn = biasStandInActive() ? biasStandInOn_ : (fs.biasTeePresent && biasTeePanel_.shown);
    fs.patchRunning = patchRunning_;
    // A radio is being opened (8.3): the converter rows are greyed meanwhile.
    fs.deviceOpening = deviceOpenPending_;
    // The auto-notch has found a tone, and the stereo decoder has locked the pilot (8.4).
    fs.autoNotchEngaged = autoNotchEngaged;
    fs.pilotLocked = pilotLocked;
    // The radio's own centre, for the CONVERTERS list to read live (8.3): one
    // atomic store a pass. (Defined beside the converter's other readbacks.)
    converterRadioCentre_.store(converterRadioCentreHz(), std::memory_order_relaxed);
    // Handed over by the front end (OPEN 4), not asked of it while publishing.
    const FrontEndFacts facts = frontEndFacts();
    // ...except whether the web server listens: the server is the engine's own since 5b, so the bit is its host's answer, not a fact the
    // window hands over.
    fs.webListening = servers_.webListening();
    fs.positionSet = rxSet_;

    r.centreHz = src.centerFrequencyHz();
    r.vfoOffsetHz = pipeline_.vfoOffsetHz();
    r.tunedHz = r.centreHz + r.vfoOffsetHz;
    r.sampleRateHz = src.sampleRateHz();
    r.channelRateHz = pipeline_.channelRateHz();
    r.bandwidthHz = vfoBandwidthHz_;
    // FIGURES KEPT AS float (the controls' own type) ARE PUBLISHED AS THE
    // DECIMAL THEY WERE SET TO (engine stage 4): a volume set to 0.4 reads
    // back 0.4, not 0.4000000059604645 - an interface comparing what it sent
    // with what landed (the API's rule 2, the web page's slider) sees the
    // same number.
    r.squelchDb = floatAsDecimal(squelchDb_);
    r.volume = floatAsDecimal(volume_);
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
    r.dbMin = floatAsDecimal(dbMin_);
    r.dbMax = floatAsDecimal(dbMax_);
    r.nrStrength = floatAsDecimal(nrStrength_);
    r.notchHz = floatAsDecimal(notchFreqHz_);
    r.notchQ = floatAsDecimal(notchQ_);
    r.pilotLevel = static_cast<double>(pipeline_.pilotLevel());
    // Where the auto-notch sits: 0 when it is not engaged (the pipeline keeps the
    // frequency of the last tone it held).
    r.autoNotchHz = autoNotchEngaged ? autoNotchHz : 0.0;
    r.demodMode = cascade::gui::abiDemodForModeIndex(modeIndex_);
    r.deemphasis = static_cast<std::uint32_t>(deemphIndex_);
    r.gainCount = device_ != nullptr ? static_cast<std::uint32_t>(deviceGainRanges_.size()) : 0u;
    r.decodersRunning = fedModules;
    r.decodersFitted = fittedModules;
    r.txMode = static_cast<std::uint32_t>(transmitter_.mode());
    r.audioUnderruns = sink.underruns();
    // WHERE IT WOULD TRANSMIT (0.3): with SPLIT off that is the receiver's
    // dial, radio open or not - the pass follows it there (pumpPublish) - so
    // a page with no radio open still letters a frequency that is true.
    r.txFrequencyHz = cascade::gui::txFrequencyHz(transmitSplit_, transmitSplitHz_, std::max(0.0, currentAbsoluteHz()));
    // THE SETTING, which is the board's readback once one is open
    // (TX_SET_POWER reads it back) - never the 0 dB (full power) a closed
    // transmitter's sink-less powerDb() answers.
    r.txPowerDb = transmitter_.sink() != nullptr ? transmitter_.powerDb() : transmitPowerDb_;
    // THE TRANSMITTER AS ITS PAGE DRAWS IT (API 0.3, docs/api-0.3-additions.md 8.1).
    {
        const cascade::source::IqSink* txSink = transmitter_.sink();
        r.txFlags = 0;
        if (transmitSplit_) { r.txFlags |= FOXAPI_TX_SPLIT; }
        if (transmitMonitor_) { r.txFlags |= FOXAPI_TX_MONITOR; }
        if (txSink != nullptr && txSink->faulted()) { r.txFlags |= FOXAPI_TX_FAULTED; }
        if (micOpen_.inFlight()) { r.txFlags |= FOXAPI_TX_INPUT_OPENING; }
        double quiet = -89.75, loud = 0.0;
        if (txSink != nullptr && txSink->gainRangeDb(quiet, loud)) { r.txFlags |= FOXAPI_TX_POWER_RANGE; }
        r.txPowerMinDb = quiet;
        r.txPowerMaxDb = loud;
        r.txInput = static_cast<std::uint32_t>(transmitInputIndex_);
        r.txSplitHz = transmitSplitHz_;
        r.txToneHz = transmitToneHz_;
        r.txInputPeak = static_cast<double>(transmitter_.takeInputPeak());
        cascade::core::formatUtf8(r.txDeviceName, sizeof(r.txDeviceName), "%s",
                                  txSink != nullptr && txSink->name() != nullptr ? txSink->name() : "");
        // Copied into the field under the Transmitter's lock: lastError()
        // would build a std::string every pass.
        transmitter_.lastErrorInto(r.txError, sizeof(r.txError));
    }
    r.txHoldRemainingMs = key.holdRemainingMs;
    r.txLatchRemainingMs = key.latchRemainingMs;
    e.txKeyOwner = key.owner;
    cascade::core::formatUtf8(r.deviceName, sizeof(r.deviceName), "%s",
                              src.name() != nullptr ? src.name() : "");
    // The sink's name try-locks and answers nothing while an open holds the
    // lock: that is not a change of sink, so the last good name is kept and
    // no counter moves (keepLastGoodName's rule, without openedDeviceName()'s
    // std::string copy every pass).
    sink.refreshOpenedDeviceName(sinkNameLastGood_);
    cascade::core::formatUtf8(r.sinkName, sizeof(r.sinkName), "%s", sinkNameLastGood_.c_str());
    cascade::core::formatUtf8(r.faultMessage, sizeof(r.faultMessage), "%s",
                              testFault_.empty() ? faultMessage.c_str() : testFault_.c_str());
    // Why the key last opened ON ITS OWN - the rules' sentence ("" when the
    // operator opened it).
    cascade::core::formatUtf8(r.txUnkeyReason, sizeof(r.txUnkeyReason), "%s", key.unkeyReason);

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
    // THE RADAR SCOPE'S POSITION (API 0.3 8.11): the same three values, in the state; the
    // coordinates are 0 with no position (they are valid with FOXAPI_RX_POSITION_SET).
    r.rxLatDeg = rxSet_ ? rxLat_ : 0.0;
    r.rxLonDeg = rxSet_ ? rxLon_ : 0.0;
    e.autoNotchEngaged = autoNotchEngaged;
    e.autoNotchFreqHz = autoNotchHz;
    e.pilotLocked = pilotLocked;
    // THE RDS DECODER (API 0.3 8.4): the snapshot the pass was handed - one read,
    // taken once for both halves of the publish - into the state's fixed fields.
    // The five that change a few times a minute move rdsSeq; the two counters
    // are measurements.
    r.rdsFlags = (rds.synced ? FOXAPI_RDS_SYNCED : 0u) | (rds.state.piValid ? FOXAPI_RDS_PI_VALID : 0u) |
                 (rds.state.psValid ? FOXAPI_RDS_PS_VALID : 0u) | (rds.state.tp ? FOXAPI_RDS_TP : 0u) |
                 (rds.state.ta ? FOXAPI_RDS_TA : 0u) | (!rds.state.radioText.empty() ? FOXAPI_RDS_RT_VALID : 0u);
    r.rdsPi = rds.state.pi;
    r.rdsPty = rds.state.pty;
    copyFixed(r.rdsPs, sizeof(r.rdsPs), rds.state.ps);
    copyFixed(r.rdsRadioText, sizeof(r.rdsRadioText), rds.state.radioText);
    r.rdsGroups = rds.state.groupsDecoded;
    r.rdsErrors = rds.state.blockErrors;
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
    // THE STATUS COLUMN AS A PAGE DRAWS IT (API 0.3, docs/api-0.3-additions.md
    // 8.2): the AUDIO / SINK / DECODER OUTPUT / DECODERS cards' figures, from
    // the same calls the column made from its frame until stage 4 - each read
    // ONCE, here, for both the /api/status extension and the state a session
    // reads. Nothing below allocates, and the runner's lock is taken once.
    {
        const double rateHz = cascade::core::Pipeline::kAudioRateHz;
        const std::uint64_t ringFrames = sink.ringFrames();
        const std::uint64_t ringCapacityFrames = sink.ringCapacityFrames();
        e.audioPrimingCallbacks = sink.primingCallbacks();
        e.audioRingMs = 1000.0 * static_cast<double>(ringFrames) / rateHz;
        e.audioRingCapacityMs = 1000.0 * static_cast<double>(ringCapacityFrames) / rateHz;
        e.audioLeadMs = cascade::sink::audioLeadMs(sink.leadFrames());
        r.audioRateHz = rateHz;
        r.audioRingFrames = ringFrames;
        r.audioRingCapacityFrames = ringCapacityFrames;
    }
    // The plugin audio path's gap counters and who holds the speakers: ONE
    // acquisition of the runner's lock (it was two, audioGaps() and
    // audioGapFrames(); the name is the third read, and the status text's own
    // publish still asks for it separately).
    const bool pluginPlaying = pluginRunner_.audioPathSnapshot(
        e.audioPluginGaps, e.audioPluginGapFrames, r.audioPlayingPlugin, sizeof(r.audioPlayingPlugin));
    r.audioPluginGaps = e.audioPluginGaps;
    r.audioPluginGapFrames = e.audioPluginGapFrames;
    {
        // The SINK card's four states, as the column asked them: a worker
        // inside the device's open first (nothing else about the sink may be
        // asked then: STREAM_ALIVE is clear while OPENING), then whether any
        // open ever succeeded, then the watchdog's view of the stream.
        const bool opening = audioOpen_.inFlight();
        r.audioFlags = 0;
        if (opening) { r.audioFlags |= FOXAPI_AUDIO_OPENING; }
        if (sink.everOpened()) { r.audioFlags |= FOXAPI_AUDIO_EVER_OPENED; }
        if (!opening && sink.streamAlive()) { r.audioFlags |= FOXAPI_AUDIO_STREAM_ALIVE; }
        if (pluginPlaying) { r.audioFlags |= FOXAPI_AUDIO_PLUGIN_PLAYING; }
        r.decodersMatched = matchedModules;
        r.decoderLinesTotal = decoderLinesTotal_;
        cascade::gui::joinMuteNamesInto(r.audioMutedBy, sizeof(r.audioMutedBy), mutedBy_);
    }
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
    // THE SCANNER AS ITS SECTION DRAWS IT (API 0.3, docs/api-0.3-additions.md 8.6): the state
    // word, where it is, the range a Start would scan (as stored, in Hz) and the four timings
    // (whole milliseconds). The settings move scanSeq; the state word and the frequency move
    // seq alone. Plain numbers: nothing allocates.
    static_assert(cascade::core::kScannerIdle == FOXAPI_SCANNER_IDLE &&
                      cascade::core::kScannerScanning == FOXAPI_SCANNER_SCANNING &&
                      cascade::core::kScannerPaused == FOXAPI_SCANNER_PAUSED &&
                      cascade::core::kScannerHolding == FOXAPI_SCANNER_HOLDING,
                  "the state words are the scanner's, as /api/status names them");
    r.scannerState = e.scannerState;
    r.scanCurrentHz = scanner_.currentHz();
    r.scanStartHz = e.scanStartHz;
    r.scanStopHz = e.scanStopHz;
    r.scanStepHz = e.scanStepHz;
    r.scanDwellMs = wholeMilliseconds(scanDwellMs_);
    r.scanHoldMs = wholeMilliseconds(scanHoldMs_);
    r.scanResumeMs = wholeMilliseconds(scanResumeMs_);
    r.scanListenMs = wholeMilliseconds(scanListenMs_);
    // THE SOURCE SECTION AS IT IS DRAWN (API 0.3 8.10): one word of conditions, each from its own member. Plain
    // reads of what the pass holds, and the scan gate's question; nothing allocates.
    r.sourceFlags = 0;
    if (soapyScanPending_) { r.sourceFlags |= FOXAPI_SRC_SCAN_PENDING; }
    if (soapyScanned_) { r.sourceFlags |= FOXAPI_SRC_SCANNED; }
    if (soapyScanPartial_) { r.sourceFlags |= FOXAPI_SRC_SCAN_PARTIAL; }
    if (soapyScanGated()) { r.sourceFlags |= FOXAPI_SRC_SCAN_GATED; }
    if (lookForNetworkUsrps_) { r.sourceFlags |= FOXAPI_SRC_NETWORK_USRPS; }
    if (cascade::gui::networkUsrpHint(soapyAbsentDrivers_) != nullptr) { r.sourceFlags |= FOXAPI_SRC_UHD_LEFT_OUT; }
    if (sdrPlayRowsFound_) { r.sourceFlags |= FOXAPI_SRC_SDRPLAY_ROWS; }
    if (soundCardListed_) { r.sourceFlags |= FOXAPI_SRC_CARD_LISTED; }
    if (soundCardScanPending_) { r.sourceFlags |= FOXAPI_SRC_CARD_SCANNING; }
    if (soundCardOpenPending_) { r.sourceFlags |= FOXAPI_SRC_CARD_OPENING; }
    e.catalogueBusy = catalogPending_ || installPending_;
    const EngineHost::BasemapFacts& basemap = facts.basemap;
    e.basemapActive = basemap.active;
    e.basemapMinZoom = basemap.minZoom;
    e.basemapMaxZoom = basemap.maxZoom;
    e.basemapTileSize = basemap.tileSize;

    // THE AIRSPY PANEL'S STATE (engine/stage3b-pre Airspy round): the ONE
    // place asAirspyDevice()'s raw pointer is still read every frame - here,
    // where the read stays on the Engine's own thread and never crosses to a
    // control. drawAirspyControls reads e.airspy* instead now.
    if (cascade::source::AirspySource* a = asAirspyDevice()) {
        e.airspyOpen = true;
        e.airspyDecimation = a->decimation();
        e.airspyHardwareSampleRateHz = a->hardwareSampleRateHz();
        e.airspyGainMode = static_cast<std::uint32_t>(a->gainMode());
        e.airspyLnaAgc = a->lnaAgc();
        e.airspyMixerAgc = a->mixerAgc();
        const std::vector<unsigned> choices = a->decimationChoices();
        e.airspyDecimationChoiceCount = static_cast<std::uint32_t>(
            std::min(choices.size(), cascade::core::AppStateExt::kMaxAirspyDecimationChoices));
        for (std::uint32_t i = 0; i < e.airspyDecimationChoiceCount; ++i) {
            e.airspyDecimationChoices[i] = choices[i];
        }
    }
}

void Engine::applyPluginApi() {
    cascade::core::PluginApiCore& api = *pluginApi_;

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
    // The legacy request_tune's queued retunes (pump move step P3): each retunes the SOURCE's centre, as the call always did,
    // here on the pass - after the grant is checked again, and only while a device is open (the call's own answer said so then;
    // it may have closed since).
    {
        std::vector<cascade::core::PluginApiCore::CentreRetune> retunes;
        api.takeCentreRetunes(retunes);
        for (const cascade::core::PluginApiCore::CentreRetune& r : retunes) {
            if (!api.centreRetuneStillAllowed(r)) { continue; }
            if (!(pipeline_.activeSource().sampleRateHz() > 0.0)) { continue; }
            retuneSourceHz(r.hz);
        }
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
                // COUNTED, one for each line actually pushed (an empty part is none): the engine API's
                // DECODER_LINES ring takes "the lines the count says are new", and an interface reads them
                // only when decoderLinesTotal moves (API 0.3 8.7, D3). An uncounted line was invisible to
                // both, and skewed the ring's slice and numbering once it sat among counted ones.
                ++decoderLinesTotal_;
                decoderLog_.push_back(std::move(d));
            }
            from = to + 1;
        }
        if (l.level >= CASCADE_LOG_WARN) {
            // The notice is part of the lists' fingerprint (API 0.3 8.8, hashed by level and text), so a plugin
            // that says the same warning every pass must change nothing: an equal replace is not written.
            const auto it = pluginNotices_.find(l.key);
            if (it == pluginNotices_.end() || it->second.level != l.level || it->second.text != l.text) {
                pluginNotices_[l.key] = PluginNotice{l.level, l.text};
            }
        }
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

void Engine::maybeAutoPresetOnStart(const std::string& pluginKey) {
    maybeAutoPreset(pluginKey, "started", /*pin=*/true);
}

void Engine::maybeAutoPresetOnShow(const std::string& pluginKey) {
    maybeAutoPreset(pluginKey, "window opened", /*pin=*/false);
}

void Engine::maybeAutoPreset(const std::string& pluginKey, const char* verb, bool pin) {
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

    CascadePreset ps = presets[static_cast<std::size_t>(idx)];
    // KEEP TUNED ON START (CASCADE_PRESET_KEEP_TUNED_ON_START, 0.99.74; stub campaign B5 - master's, lost with the rest of this function's lines): the
    // plugin asks for its reception SETTINGS (mode, bandwidth, de-emphasis) at the frequency the receiver is on now, not a retune to its preset's.
    if ((ps.flags & CASCADE_PRESET_KEEP_TUNED_ON_START) != 0u) {
        ps.frequencyHz = deviceCentreHz + vfoOffsetHz;
        ps.flags &= ~CASCADE_PRESET_DEVICE_CENTRE;
    }
    // THE IDENTICAL PATH THE BUTTON TAKES: mode, bandwidth, device rate, the
    // tune itself and the plugin's own windows. Starting a decoder (or
    // opening its window) is meant to feel like pressing its preset for it,
    // not a cut-down copy of doing so.
    applyPluginPreset(*found, ps, pin);

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
    return pluginMutesFor(p, cascade::core::pluginKey(p));
}

bool Engine::pluginMutesFor(const cascade::core::LoadedPlugin& p, const std::string& key) const {
    const bool def = cascade::core::muteDefaultForCaps(p.capabilities);
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
    if (!pipeline_.pluginAudioMuted()) { return {}; }
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
        // API 0.3 8.7: tell the interfaces that asked (FOXAPI_EVENT_MUTE_TUNE_AWAY), with how many plugins the
        // question is about. Sent by the next pass's endPass, after the publish that names them.
        api_->muteTuneAway(static_cast<std::uint32_t>(mutePopup_.names.size()));
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
    // same audio. A data decoder suppresses only the demodulated carrier;
    // its own decoded programme must remain audible through that suppression.
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
    pipeline_.setDemodAudioMuted(muted);
    pipeline_.setAudioMuted(userMuted_ || txMute);
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
    // (The track-info plugin's status lines ("cannot reach the registry") reach the same log as a COMMAND since the pump
    // move's step P2 - FOXAPP_OP_TRACK_INFO_LINE, sent by the window that owns the plugin's cache and polls its text on its
    // own thread; this pass used to ask the window for them through a host hook, on whichever thread it ran.)
    // THE PATCH'S DECODERS, drained every frame for the same reason the
    // runner's are: its queue is bounded and drops. A line is SHOWN only when
    // its node's Text output is wired to a Text sink - that wire is what the
    // Text out part is for, and a decoder wired to nothing is marked on the
    // canvas as having nobody listening. Tagged with the NODE's name, so two
    // POCSAG decoders on two frequencies read as two sources, not one.
    // Pictures: the newest per node, kept (takePatchImage) for the face an interface draws from read_image.
    // From the receiver's patch runner and from EVERY patch radio's own
    // (0.99.17) - a decoder runs on whichever radio it is wired to.
    std::vector<cascade::core::patch::Runner*> patchRunners{&pipeline_.patchRunner()};
    for (auto& [rid, radio] : patchRadios_) {
        (void)rid;
        patchRunners.push_back(&radio->runner());
    }
    std::vector<cascade::core::patch::PatchLine> patchLines;
    for (cascade::core::patch::Runner* pr : patchRunners) {
        for (auto& [node, img] : pr->drainImages()) { takePatchImage(node, std::move(img)); }
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
        // A line is in the backlog only when its Text output reaches a Text out; WHICH Text outs show it is the
        // interface's to route (DECODER_LINES' value[0] names the decoder; the engine no longer keeps a copy per Text out).
        if (!patchDecoderIsShown(pl.node)) { continue; }
        ++decoderLinesTotal_;
        cascade::core::DecodedLine l;
        l.node = pl.node;   // (API 0.3 8.12: DECODER_LINES value[0] - which patch decoder said it)
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
    if (txSinkForTest_ != nullptr && transmitArgs_ == "test:tx:dummy") {
        // TESTS ONLY: the table's dummy load (testHooks=1), no RF anywhere.
        transmitter_.setSink(txSinkForTest_());
        transmitter_.setMode(cascade::dsp::txModeFromIndex(transmitModeIndex_));
        transmitter_.setInput(cascade::core::TxInput::Tone);
        transmitter_.setDataFlat(txDataFlat_);
        if (txArmRefusal_.load(std::memory_order_seq_cst) == cascade::core::TxArming::Refusal::NoRadio) { txArmRefusal_.store(cascade::core::TxArming::Refusal::None, std::memory_order_seq_cst); }
        return;
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
    transmitter_.setDataFlat(txDataFlat_);
    // An ARM refusal that said "no transmitter is open" is answered by this (stub campaign B13).
    if (txArmRefusal_.load(std::memory_order_seq_cst) == cascade::core::TxArming::Refusal::NoRadio) { txArmRefusal_.store(cascade::core::TxArming::Refusal::None, std::memory_order_seq_cst); }
    followTransmitFrequency();
}

void Engine::closeTransmitRadio() {
    // setSink(nullptr) stops and destroys whatever is there, which silences
    // it: the driver's stop() and its destructor both do, and Transmitter
    // unkeys before either runs.
    transmitter_.setSink(nullptr);
    transmitError_.clear();
    // The radio is gone, and so is the operator's authorisation to key it: a window armed over one board does not carry over to the next one opened (stub campaign B13).
    txPluginEnd("the transmit radio was closed");
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
    if (!detachAndUnloadPlugins()) {
        deferPluginUnload(PluginUnloadOp::RemoveBlocked, fileName);
        return;
    }
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
    // (bandPlanDirForTest_ is the test.bandPlanDir hook's: the shipped plans
    // sit beside cascade.exe, which no test executable is.)
    const std::string dir =
        bandPlanDirForTest_.empty() ? cascade::core::BandPlan::defaultDir() : bandPlanDirForTest_;
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
    // The recording's start, stop and arming are the scope `recorder` wherever they are called from (0.99.64, core/frame_timing.hpp; stub campaign B18).
    cascade::core::OwnedFrameScope recorderScope(cascade::core::FrameScope::Recorder);
    // A start still opening its file is withdrawn first: nothing is recording yet, so there is no tap to take out and no header to patch, and
    // nothing here waits for the disk (RecordStart::cancel only sets a flag - the file, when it arrives, is closed unused by
    // pollRecordStarts). A no-op when no open is out.
    iqStart_.cancel();
    // Order per the Pipeline::setIqRecorder contract: after the setter returns no writeIq against this recorder is in flight or can begin
    // (the pointer swap serializes on the mutex the DSP thread holds across writes), so the finish - which patches the header and closes
    // the file - cannot overlap a write. Both calls are no-ops when already idle.
    //
    // THE FINISH IS NOT DONE HERE (0.99.65): finishTake detaches the file from the recorder without touching the disk and a worker flushes,
    // patches and closes it. A Record queued behind a finish still out is withdrawn too.
    iqStartQueued_ = false;
    pipeline_.setIqRecorder(nullptr);
    finishTake(true);
}

void Engine::stopAudioRecording() {
    cascade::core::OwnedFrameScope recorderScope(cascade::core::FrameScope::Recorder);
    audioStart_.cancel();  // see stopIqRecording
    audioStartQueued_ = false;
    pipeline_.setAudioRecorder(nullptr);
    finishTake(false);
}

namespace {
// What the worker said, said once, in the log and nowhere else: the synchronous stop said nothing at all about a header that did not reach
// the file (it ignored every result), and this keeps the screen the same - a line the user cannot act on, about a file that still opens -
// while no longer losing the fact. Never names the file.
void collectFinish(std::shared_ptr<cascade::core::RecordFinisher::Ticket>& slot, const char* what) {
    if (!slot || !slot->done.load()) { return; }
    if (!slot->ok.load()) {
        cascade::core::diagWarnf("recorder: the %s take's file could not be finalised - it keeps the header it was opened with", what);
    }
    slot.reset();
}
}  // namespace

void Engine::finishTake(bool iq) {
    cascade::core::Recorder& rec = iq ? iqRecorder_ : audioRecorder_;
    cascade::core::Recorder::FinishRequest req;
    // False when there is no open file: never started, already stopped, or ended by the size limit (which closed its own file). Nothing to
    // finish, nothing to wait for.
    if (!rec.stopForFinish(req)) { return; }
    (iq ? iqFinish_ : audioFinish_) = cascade::core::RecordFinisher::submit(std::move(req));
}

bool Engine::takeFinishing(bool iq) const {
    const auto& slot = iq ? iqFinish_ : audioFinish_;
    return slot && !slot->done.load();
}

void Engine::pollRecordFinishes() {
    const auto service = [this](bool iq) {
        collectFinish(iq ? iqFinish_ : audioFinish_, iq ? "I/Q" : "audio");
        bool& queued = iq ? iqStartQueued_ : audioStartQueued_;
        if (queued && !takeFinishing(iq)) {
            queued = false;
            if (iq) {
                startIqRecording();
            } else {
                startAudioRecording();
            }
        }
    };
    service(true);
    service(false);
}

bool Engine::drainRecordFinishes(std::chrono::steady_clock::time_point deadline) {
    // A Record queued behind a finish is not started at quit: the window is closing.
    iqStartQueued_ = false;
    audioStartQueued_ = false;
    const bool landed = cascade::core::RecordFinisher::drain(deadline);
    if (!landed) {
        cascade::core::diagLogf(
            "recorder: a take's file was still being closed at exit - the disk did not answer in time; it keeps the header it was opened with");
    }
    collectFinish(iqFinish_, "I/Q");
    collectFinish(audioFinish_, "audio");
    return landed;
}

void Engine::pollRecordStarts(double nowS) {
    cascade::core::OwnedFrameScope recorderScope(cascade::core::FrameScope::Recorder);
    // Both takes, the same way: collect a finished open and act on it, then say in the log - once - if one is taking long. A no-op on every
    // pass nothing was requested, so it costs nothing to call unconditionally. `nowS` is the take's start time (the elapsed-time readout's
    // origin): the pass passes its own clock.
    const auto service = [this, nowS](bool iq, cascade::gui::RecordStart& start, const char* what) {
        cascade::gui::RecordStart::Result r;
        if (start.poll(r)) { finishRecordStart(iq, r, nowS); }
        // THE ONLY RECORD A REPORT WILL CARRY of a disk that was slow to open a recording: once when an open has been out for 5 s, once
        // when it comes back. Neither names the directory or the file, which are in the user's profile.
        double seconds = 0.0;
        switch (start.takeNotice(seconds)) {
            case cascade::gui::RecordStart::Notice::Stuck:
                cascade::core::diagWarnf(
                    "recorder: the %s file has not opened for %.0f s - the open is waiting on a worker thread, the pass is not", what, seconds);
                break;
            case cascade::gui::RecordStart::Notice::Recovered:
                cascade::core::diagLogf("recorder: the %s file opened after %.0f s", what, seconds);
                break;
            case cascade::gui::RecordStart::Notice::None:
                break;
        }
    };
    service(true, iqStart_, "I/Q");
    service(false, audioStart_, "audio");
    // A Record that was pressed behind a take's finish, started now its file is closed.
    pollRecordFinishes();
}

void Engine::finishRecordStart(bool iq, cascade::gui::RecordStart::Result& r, double nowS) {
    cascade::core::Recorder& rec = iq ? iqRecorder_ : audioRecorder_;
    const char* what = iq ? "I/Q" : "audio";
    if (r.cancelled) {
        // Stopped while the file was opening: RecordStart has already closed it, and nothing was armed or tapped. Silent on screen - the
        // user pressed Stop, and "the recording ended because you stopped" is not news.
        cascade::core::diagLogf("recorder: the %s take was stopped before its file finished opening", what);
        return;
    }
    if (!r.ok) {
        // The same words Recorder::start gave, a little later.
        recordError_ = r.error;
        return;
    }
    if (iq && pipeline_.inputRateHz() != iqRecordRateHz_) {
        // THE RATE MOVED WHILE THE FILE WAS OPENING (a source switch, a SoapySDR rate change): the header on that file is for the old rate,
        // and a take whose header disagrees with its samples replays detuned - the reason an accepted rate change ends a take already
        // running. Not started, and said, because nothing went wrong that the user did not do.
        r.file = cascade::core::Recorder::OpenedFile{};  // closes it, unused
        // Translated where it is set, like the other notes the engine keeps in a member. The log line below is not.
        recordNotice_ =
            tr("The I/Q recording was not started because the input rate changed while its "
               "file was opening; press Record to start a new one.");
        cascade::core::diagLogf("recorder: the I/Q take was dropped, the input rate changed while its file opened");
        return;
    }
    std::string err;
    // ARM FIRST, TAP SECOND - the Pipeline::set*Recorder contract: the tap must never feed a recorder that is not accepting, and begin() is
    // what makes it accept. No write can be in flight against it yet, because the pipeline has not been given it.
    if (!rec.begin(std::move(r.file), err)) {
        recordError_ = err;
        return;
    }
    recordError_.clear();
    recordNotice_.clear();
    if (iq) {
        iqRecordStartS_ = nowS;
        pipeline_.setIqRecorder(&iqRecorder_);
    } else {
        audioRecordStartS_ = nowS;
        pipeline_.setAudioRecorder(&audioRecorder_);
    }
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

void Engine::holdAudioLockForTest() {
    // The seam of FOXSDR_HOLD_AUDIO_LOCK (0.99.68, bounded runs only, see holdAudioLockMs_ in the header): a thread of this process takes
    // the audio mutex - the one the DSP thread holds across a block - and keeps it for that many milliseconds, standing in for a plugin that
    // stretched a block. Once, and logged only when the mutex really is held, so the test can tell a hold that happened from one that did
    // not.
    holdAudioLockDone_ = true;
    holdAudioLockThread_ = std::thread([this] {
        pipeline_.holdLockForTest(cascade::core::Pipeline::LockForTest::Audio, holdAudioLockMs_, &holdAudioLockAcquired_);
    });
    for (int i = 0; i < 250 && !holdAudioLockAcquired_.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (holdAudioLockAcquired_.load(std::memory_order_acquire)) {
        cascade::core::diagLogf("test: holding the audio lock for %d ms", holdAudioLockMs_);
    } else {
        cascade::core::diagLogf("test: the audio lock could not be taken");
    }
}

void Engine::stopReceiver(const char* who) {
    // Play-stop while recording stops the recording cleanly (spec): taps
    // uninstalled and both WAVs finalized BEFORE the DSP threads join, so a
    // take can never outlive the sample flow it was taping. Only when the
    // receiver is running or faulted - see the header for why a stop that
    // stops nothing leaves an armed take alone. Silent, as it always was: the
    // user pressed Stop, and "the recording ended because you stopped" is
    // not news.
    //
    // ONLY A TAKE THAT IS RUNNING IS ENDED (0.99.68). Each recording stop swaps the pipeline's recorder pointer under audioMutex_, the mutex
    // the DSP thread holds across a whole block, so with nothing recording a stop still queued twice behind the block in progress - and a
    // plugin-heavy block runs for hundreds of milliseconds. The 0.99.65 fast-fail's log: a 740 ms frame, 701 ms of it in recorder, four
    // seconds in, nothing recording, eight seconds before the death. An idle recorder has no tap to take out and no file to finish, so that
    // wait is not spent. The stop still waits once, inside pipeline_.stop(), for the block in progress to end - that wait is the join's.
    const bool running = pipeline_.running() || pipeline_.faulted();
    const bool iq = running && (iqRecorder_.recording() || iqStart_.pending() || iqStartQueued_);
    const bool audio = running && (audioRecorder_.recording() || audioStart_.pending() || audioStartQueued_);
    // The two recording stops timed, for the log line below: the quantity the 0.99.65 report showed as 701 ms. tests/test_stop_ends_
    // recordings.cpp asserts on it.
    const auto recordersFrom = std::chrono::steady_clock::now();
    if (iq) { stopIqRecording(); }
    if (audio) { stopAudioRecording(); }
    const long long recordersMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - recordersFrom).count());
    // WHO ASKED, in the log (0.99.68): that same report's log showed the stop's cost and nothing of its cause. One line, before the stop
    // itself, so it is there even if the stop is the last thing the session does. It ends with what the recorders' part of the stop cost.
    cascade::core::diagLogf("receiver: stop requested by %s; %s (recorders %lld ms)", who,
                            !running        ? "the receiver was not running"
                            : (iq && audio) ? "ending the I/Q and audio recordings"
                            : iq            ? "ending the I/Q recording"
                            : audio         ? "ending the audio recording"
                                            : "no recording to end",
                            recordersMs);
    // Unconditional, as the stop of a control request always was (the dome,
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
    cascade::core::OwnedFrameScope recorderScope(cascade::core::FrameScope::Recorder);
    // A take that is recording, or whose file is still opening, is left exactly as it is: a second press of the button, the key or the
    // browser's Record changes nothing (RecordStart refuses a second open anyway, so two cannot race for one file name). One whose start was
    // withdrawn and has not yet come back is neither, and a Record in that gap is not a take.
    if (audioRecorder_.recording() || audioStart_.pending()) { return !audioStart_.cancelled(); }
    // THE PREVIOUS TAKE'S FILE IS STILL BEING CLOSED (a worker's, 0.99.65): this press is remembered and the take starts on the pass that
    // file is closed (pollRecordFinishes). Not started now - a file name has a one-second resolution, and an open beside a file still being
    // finalised could truncate it - and not refused either, so a Stop followed by a Record in one breath (a script, the web remote) still
    // gives the take that was asked for.
    if (takeFinishing(false)) {
        audioStartQueued_ = true;
        return true;
    }
    // THE DISK IS NOT TOUCHED HERE. This used to be Recorder::start, whose create_directories and fopen waited for the disk on this thread,
    // and a slow one froze it (see gui/record_start.hpp). The cheap half stays: a refusal that needs no disk is reported at once.
    std::string err;
    cascade::core::Recorder::OpenRequest req;
    if (!audioRecorder_.prepare(cascade::core::RecordKind::Audio, recordDir_, cascade::core::Pipeline::kAudioRateHz, std::string{}, req,
                                err)) {
        recordError_ = err;
        return false;
    }
    audioStart_.request(audioRecorder_.opener(), std::move(req));
    return true;
}

bool Engine::startIqRecording() {
    cascade::core::OwnedFrameScope recorderScope(cascade::core::FrameScope::Recorder);
    // The Record IQ key's body and the web remote's recordIq. Baseband at the DSP input rate through the pipeline's raw tap.
    // See startAudioRecording for the pending and withdrawn cases.
    if (iqRecorder_.recording() || iqStart_.pending()) { return !iqStart_.cancelled(); }
    // See startAudioRecording: behind a finish still out, remembered.
    if (takeFinishing(true)) {
        iqStartQueued_ = true;
        return true;
    }
    const double rate = pipeline_.inputRateHz();
    std::string err;
    cascade::core::Recorder::OpenRequest req;
    if (!iqRecorder_.prepare(cascade::core::RecordKind::BasebandIq, recordDir_, rate, std::string{}, req, err)) {
        recordError_ = err;
        return false;
    }
    // The rate the file's header is being written for, kept to compare with the rate when the file arrives: a take is one rate, and a source
    // that changed it while the disk was slow must not get a header that lies (see finishRecordStart).
    iqRecordRateHz_ = rate;
    iqStart_.request(iqRecorder_.opener(), std::move(req));
    return true;
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
            commitModeIndex(m);
            break;
        }
    }
    // Same clamp as the config restore: [3 kHz, 90% of channel rate].
    const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
    const double bwBefore = vfoBandwidthHz_;
    vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(b.bandwidthHz, bwHi));
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    // -1 for a bookmark saved at a bandwidth the list does not carry (one
    // taken while a preset had the VFO at 40 kHz, say): the combo letters the
    // real figure and ticks nothing.
    bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
    if (vfoBandwidthHz_ != bwBefore) { logBandwidthChange("bookmark"); }
}

std::uint64_t Engine::addBookmarkHere(const std::string& name, const std::string& group) {
    cascade::core::Bookmark b;
    b.name = name;
    b.group = group;   // "" = ungrouped, as every bookmark was before master's 0.99.93
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
    const int at = freqMgr_.add(std::move(b));
    saveBookmarks();
    return at >= 0 && static_cast<std::size_t>(at) < freqMgr_.list().size()
               ? freqMgr_.list()[static_cast<std::size_t>(at)].id
               : 0;
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
    // LIST MODE (stub campaign B6): the ticked rows of the frequency list stand in for the range.
    if (scanTicked_) { p.list = tickedScanList(); }
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
    if (kind == "open-rtltcp") {
        openRtlTcpAt(args);
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
        rd.idle = pluginIsIdle(rd.key);
        runnable.push_back(std::move(rd));
    }
    return runnable;
}

// THE ONE PLACE modeIndex_ IS ASSIGNED (0.99.62): the diagnostics context names the mode, so the write and the block's render are one
// step - a new writer that went round this would leave a crash report describing the wrong mode (tests/test_diag_context_app.cpp).
void Engine::commitModeIndex(int index) {
    if (index < 0 || index >= 8) { return; }
    modeIndex_ = index;
    pipeline_.setDemodMode(kModeMap[index]);
    refreshDiagContext();
}

// THE LINE THAT WAS MISSING. Three 0.99.59 reports said the bandwidth control did nothing, and the log could not say whether it had ever
// been used: a mode change wrote "mode: AM, bandwidth 10000" and a bandwidth change wrote nothing at all. Bandwidth only, never a
// frequency (see setModeIndex).
void Engine::logBandwidthChange(const char* how) {
    cascade::core::diagLogf("bandwidth: %.0f Hz in %s (%s)", vfoBandwidthHz_, kModeNames[modeIndex_], how);
}

void Engine::setBandwidthIndex(int index) {
    if (index < 0 || index >= kBwCount) { return; }
    const double before = vfoBandwidthHz_;
    bandwidthIndex_ = index;
    vfoBandwidthHz_ = kBwHz[index];
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    // A pick of the step already in force changes nothing and says nothing.
    if (vfoBandwidthHz_ != before) { logBandwidthChange("bandwidth list"); }
}

void Engine::setModeIndex(int index) {
    if (index < 0 || index >= 8) { return; }
    commitModeIndex(index);
    bandwidthIndex_ = kModeDefaultBw[index];
    vfoBandwidthHz_ = kBwHz[bandwidthIndex_];
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    // The MODE and its bandwidth - a demodulator change, not a tuning change.
    // No frequency reaches the log, here or anywhere else.
    cascade::core::diagLogf("mode: %s, bandwidth %.0f", kModeNames[index], vfoBandwidthHz_);
}

// THE TAIL OF AN IMPORT, on the pass that collects the worker's read (engine_disk_work.cpp): the rows join the list, the note and the log say what
// happened. `path` is the one the read was asked for (the quotes taken off), `readMs` the worker's time.
bool Engine::finishBookmarkImport(cascade::core::ImportResult r, const std::string& p, double readMs) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!r.error.empty() && r.items.empty()) {
        // THE REASON IN THE USER'S LANGUAGE (0.99.80, stub campaign B8 - master's window said it so): the core's three reasons are English
        // sentences, which put an English tail on "Could not import: ..."; the log below keeps the core's own words. Anything else passes
        // through as it was.
        std::string reason = r.error;
        if (r.error.rfind("cannot open", 0) == 0) {
            reason = cascade::core::formatText(tr("the file \"%s\" could not be opened"), p.c_str());
        } else if (r.error.rfind("cannot read", 0) == 0) {
            reason = cascade::core::formatText(tr("the file \"%s\" could not be read"), p.c_str());
        } else if (r.error.rfind("no line had a usable frequency", 0) == 0) {
            reason = tr("no line of the file had a usable frequency");
        }
        bookmarkImportNote_ = cascade::core::formatText(tr("Could not import: %s"), reason.c_str());
        // The file's KIND, never its name or path: "never the name or path of
        // a file you opened" (PRIVACY.md), and a frequency list's name is
        // usually what is on it.
        // importFrequencyFile's "cannot open" names the path; the log does not.
        std::string why = r.error;
        for (std::size_t at = why.find(p); !p.empty() && at != std::string::npos;
             at = why.find(p, at)) {
            why.replace(at, p.size(), "(the file)");
        }
        // (The extension through a UTF-8 path: .string() of a path with a letter outside the ANSI page throws on Windows.)
        const auto extension = std::filesystem::path(std::u8string(p.begin(), p.end())).extension().u8string();
        const std::string kind(extension.begin(), extension.end());
        cascade::core::diagWarnf("bookmarks: an import (%s) failed: %s", kind.c_str(), why.c_str());
        return false;
    }
    const std::size_t found = r.items.size();
    const std::size_t added = freqMgr_.addMany(std::move(r.items));
    const double ms =
        readMs + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
    return true;
}

void Engine::saveBookmarks() {
    if (bookmarkPath_.empty()) { return; }  // hermetic run: never touch disk
    bookmarkSaveDirty_ = true;
    bookmarkSaveDueS_ = clockS() + 1.0;
}

void Engine::flushBookmarkSave(bool force) {
    // THE LAST SAVE'S ANSWER, whenever it has come back (0.99.64: the write is on a worker, so what it says is a pass or more late). A
    // failure is shown in the words the synchronous save gave; a success clears a stale error.
    {
        bool ok = false;
        std::string err;
        if (bookmarkSaver_.poll(ok, err)) {
            if (ok) {
                bookmarkError_.clear();  // a successful save clears a stale error
            } else {
                bookmarkError_ = err;
            }
        }
    }
    if (!bookmarkSaveDirty_ || bookmarkPath_.empty()) { return; }
    if (!force && clockS() < bookmarkSaveDueS_) { return; }
    bookmarkSaveDirty_ = false;
    // The snapshot is taken on this thread, so the worker never reads the list being edited; the write itself - the folder, the temp file, the
    // flush, the rename - is the worker's. NEVER WAITS FOR THE DISK.
    bookmarkSaver_.request(bookmarkPath_, freqMgr_.serialize());
}

void Engine::flushMarkerSave(bool force) {
    // The last save's answer first, as flushBookmarkSave does (0.99.64).
    {
        bool ok = false;
        std::string err;
        if (markerSaver_.poll(ok, err)) {
            if (ok) {
                markerError_.clear();
            } else {
                markerError_ = err;
            }
        }
    }
    if (markerPath_.empty()) { return; }  // hermetic run: never touch disk
    const unsigned v = freqMarkers_.version();
    if (v == markerSavedVersion_) {
        markerSaveDueS_ = -1.0;
        return;
    }
    const double now = clockS();
    if (!force) {
        // Half a second after a change is first seen: a note is typed a character at a time, and costs at most two writes a second rather
        // than one a keystroke.
        if (markerSaveDueS_ < 0.0) {
            markerSaveDueS_ = now + 0.5;
            return;
        }
        if (now < markerSaveDueS_) { return; }
    }
    // The text is taken here; the write is the worker's (see flushBookmarkSave).
    markerSaver_.request(markerPath_, freqMarkers_.serialize());
    // Recorded either way: a save that failed is reported, not retried every frame; the next change tries again.
    markerSavedVersion_ = v;
    markerSaveDueS_ = -1.0;
}

bool Engine::drainListSaves(std::chrono::steady_clock::time_point deadline) {
    // What is left of the one deadline, never negative: a write that has already finished is still collected at zero
    // (ConfigWriter::finishOrAbandon).
    const auto now = std::chrono::steady_clock::now();
    const auto left = now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) : std::chrono::milliseconds(0);
    bool landed = true;
    if (!bookmarkSaver_.finishOrAbandon(left)) {
        cascade::core::diagLogf("bookmarks: final save abandoned - the disk did not answer in time");
        landed = false;
    } else if (!bookmarkSaver_.lastOk() && bookmarkSaver_.completed() > 0) {
        bookmarkError_ = bookmarkSaver_.lastError();
        cascade::core::diagWarnf("bookmarks: the final save failed");
        landed = false;
    }
    if (!markerSaver_.finishOrAbandon(left)) {
        cascade::core::diagLogf("markers: final save abandoned - the disk did not answer in time");
        landed = false;
    } else if (!markerSaver_.lastOk() && markerSaver_.completed() > 0) {
        markerError_ = markerSaver_.lastError();
        cascade::core::diagWarnf("markers: the final save failed");
        landed = false;
    }
    return landed;
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
    // A MANUAL TUNE gives an audio-output decoder its first chance to play (an independent wake signal before it has an instance to
    // report as the playing plugin); the pass's lifecycle step consumes it (engine_plugin_run.cpp).
    if (pipeline_.running() && !scanner_.active() && !isPluginPreset) { audioOutputTunePending_ = true; }
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
    // A sound card is not named (its name() carries the card's own name).
    cascade::core::diagLogf("source: the %s refused a tune %s",
                            sourceKind_ == "soundcard" ? "sound card" : pipeline_.activeSource().name(),
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
                            sourceKind_ == "soundcard" ? "sound card" : pipeline_.activeSource().name(),
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
    // LIST MODE FOLLOWS THE TICKS (stub campaign B6): a row ticked or unticked while the scan runs re-reads the list (the reconfigure restarts at
    // the first entry, which is the Scanner's rule for new parameters). Any other edit - a star, a bookmark added elsewhere, a heard time written
    // - leaves the scan where it is, or it would be pulled off the signal it is holding. Nothing ticked: stop.
    if (scanTicked_ && freqMgr_.version() != scanListVersion_) {
        scanListVersion_ = freqMgr_.version();
        const std::vector<double> now = tickedScanList();
        if (now.empty()) {
            scanner_.stop();
            return;
        }
        if (now != scanList_) { configureScanner(); }
    }

    const bool squelchOpen = pipeline_.signalPowerDb() > squelchDb_;
    const std::optional<double> retune =
        scanner_.tick(clockS() * 1000.0, squelchOpen);
    if (retune.has_value() && scanTicked_) {
        // THE ROW ITSELF, so its mode and bandwidth come with it - an airband row is AM at 10 kHz, a broadcast row WFM.
        cascade::core::Bookmark row;
        bool found = false;
        for (const cascade::core::Bookmark& b : freqMgr_.list()) {
            if (b.scan && b.freqHz == *retune) {
                row = b;
                found = true;
                break;
            }
        }
        if (found) {
            tuneToBookmark(row);
        } else {
            tuneAbsoluteHz(*retune);
        }
        scannerExpectedAbsHz_ = currentAbsoluteHz();
        scannerHasExpected_ = true;
    } else if (retune.has_value()) {
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
    // (test.rds stands in for the decoder's snapshot in a hermetic engine.)
    const cascade::core::RdsSnapshot rds = testRdsOn_ ? testRds_ : pipeline_.rdsSnapshot();
    cascade::core::PublishedState ps{};
    fillPublishedState(ps, faultMessage, rds);
    // The fault sentence the list block carries whole (API 0.3 8.10): the one the state's faultMessage cut. Assigned
    // into a string that keeps its room, so a settled pass allocates nothing.
    apiFaultText_ = testFault_.empty() ? faultMessage : testFault_;
    // The engine API's lists, handed over BEFORE the state whose listSeq
    // says they changed.
    ps.app.listsFingerprint = publishApiLists();
    std::shared_ptr<cascade::net::RadioStatus> lists = std::make_shared<cascade::net::RadioStatus>();
    std::vector<std::uint64_t> bookmarkIds;
    fillStatusLists(*lists, faultMessage, rds, bookmarkIds);
    receiverSnapshot_->publish(ps, std::move(lists), std::move(bookmarkIds));
    // (The spectrum the browser long-polls is the table's read_spectrum on the web server's session, filled by endPass - stage 5e; it was
    // the window's copy of what it drew until then.)
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
    s.tunerDisplayStyle = frontEndFacts().tunerDisplayStyle;   // handed over (OPEN 4)
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
    // IN THE PUBLISHED BLOCK with these rows (the web server's session reads
    // them from readFull() when a request is POSTED, stage 5d - it was the
    // pass at apply time before), so the map and the rows /api/status serves
    // are always from the same publish.
    {
        const std::vector<cascade::core::Bookmark>& all = freqMgr_.list();
        bookmarkIds.clear();
        for (const std::size_t i : freqMgr_.nearestSubset(currentAbsoluteHz(), 300, 100)) {
            const cascade::core::Bookmark& b = all[i];
            bookmarkIds.push_back(b.id);
            s.bookmarks.push_back({b.name, b.freqHz, b.mode, b.bandwidthHz});
        }
    }

    const std::shared_ptr<const TrackFeed> extras = trackFeed();
    // THE TRACKS ARE THE WINDOW'S POLL, AS IT HANDED THEM OVER (TrackFeed::polled; pump move step P3): the plugin UI is the
    // window's and its poll rewrites its vectors every frame, so this pass reads an immutable copy and never the vector itself.
    // With no window there is no feed, and no tracks (stage 7 decides whether a headless engine polls the plugin UI itself).
    static const std::vector<cascade::core::HostTrack> kNoTracks;
    for (const cascade::core::HostTrack& t : extras != nullptr ? extras->polled : kNoTracks) {
        // THE SAME STALENESS RULE THE DESKTOP MAP APPLIES, so the two views do
        // not disagree about what is still flying. A dropped target simply
        // stops appearing in the snapshot; ageMs is still published for the
        // ones that remain, so the web page is free to fade them on its own
        // terms - that presentation is the web UI's decision, not this one's.
        // (The WHOLE track, flags included - stub campaign B16, 0.99.78: an aircraft whose position the source carried forward stays for 180 s, here as on the map.)
        if (!cascade::core::trackPresentation(t.t).visible) { continue; }
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
        // Enrichment from the track-info plugin, as the window handed it over (FrontEndFacts::trackFeed, pump move step P2):
        // the window asks the plugin's cache for every live target once a frame - which is what drives the lookups, the ask
        // being non-blocking, PENDING costing nothing and answers cached - and this pass only looks the answer up.
        if (extras != nullptr) { applyTrackFeedInfo(*extras, w); }
        s.tracks.push_back(std::move(w));
    }

    // The receiver plugins' decoded pictures (headers only; the pixels are served by the web server's own set). THE ENGINE'S OWN since stage
    // 5e (ServerHost's image feed polls the decoders and encodes the pictures): these are the headers of the set the server holds, published
    // after it - never ahead of the bitmap a page would fetch for the revision they name.
    if (const auto headers = servers_.images().headers(); headers != nullptr) {
        s.images.insert(s.images.end(), headers->begin(), headers->end());
    }

    s.basemap.attribution = frontEndFacts().basemap.attribution;   // handed over (OPEN 4)

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
        // The browser is told what the install request would be told (0.99.74): a site's plugin is fitted from the desktop store only, so its
        // key is greyed with the same reason (and the request is refused with it: applyCommand's STORE_INSTALL).
        if (!e.source.empty()) { c.blockedReason = kSitePluginDesktopOnly; }
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
    lc.sourceGen = sourceGen_.load();
    {
        std::lock_guard<std::mutex> lk(localCommandsMutex_);
        localCommands_.push_back(std::move(lc));
    }
    // A SUBMIT WAKES THE CONTROL THREAD (pump move P4): with the pass on a thread of its own a command that waited for the next
    // period's pass would land up to one period late, which is the whole of the latency gate's allowance (the survey's 1.6 F). The
    // lock is not held: the pass that takes the wake finds the command in the queue because the push is done. A no-op with no
    // control thread (the pass is then the caller's frame).
    wakeControl();
}

void Engine::postToPass(std::function<void()> work) {
    if (!work) { return; }
    {
        std::lock_guard<std::mutex> lk(passWorkMutex_);
        passWork_.push_back(std::move(work));
    }
    wakeControl();
}

bool Engine::runOnPass(const std::function<void()>& work) {
    if (!work) { return true; }
    if (!controlThreadRunning() || controlStepped_.load() || onControlThread()) {
        work();
        return true;
    }
    // Free-running: posted, with a state the caller can cancel under the same lock the work runs under, so work that captures the
    // caller's locals can never run after the caller has left.
    struct Slot {
        std::mutex m;
        bool done = false;
        bool cancelled = false;
    };
    auto slot = std::make_shared<Slot>();
    postToPass([slot, &work] {
        std::lock_guard<std::mutex> lk(slot->m);
        if (slot->cancelled) { return; }
        work();
        slot->done = true;
    });
    while (true) {
        {
            std::lock_guard<std::mutex> lk(slot->m);
            if (slot->done) { return true; }
        }
        if (!runPassNow(std::chrono::milliseconds(2000))) {
            std::lock_guard<std::mutex> lk(slot->m);
            if (slot->done) { return true; }
            if (!controlThreadRunning()) {
                slot->cancelled = true;
                return false;
            }
        }
    }
}

void Engine::runPassWork() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(passWorkMutex_);
        if (passWork_.empty()) { return; }
        batch.swap(passWork_);
    }
    for (const std::function<void()>& w : batch) {
        w();
        passWorkRan_.fetch_add(1);
    }
}

void Engine::drainLocalCommands() {
    // TAKEN, THEN APPLIED: a command's own effects may queue more (none does
    // today); those wait for the next drain rather than extend this one. The
    // lock covers the swap only - a submit never waits for a command to be
    // applied.
    std::vector<LocalCommand> batch;
    {
        std::lock_guard<std::mutex> lk(localCommandsMutex_);
        if (localCommands_.empty()) { return; }
        batch.swap(localCommands_);
    }
    localCommandsTaken_ += batch.size();
    for (const LocalCommand& lc : batch) {
        // A radio command asked of a radio that has gone since (closed, or
        // replaced by one that opened in between - sourceGen_ moves with
        // every device_ change, including one made earlier in this batch) is
        // dropped: it was drawn for that radio, never for this one.
        if (lc.sourceGen != sourceGen_ && isDeviceScoped(lc.q.c.op)) { continue; }
        commandFromWindow_ = true;
        (void)applyCommand(lc.q.c, lc.q.longText);
        commandFromWindow_ = false;
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
                testFault_.clear();  // a fault latches until the next start (API.md 6.1)
                startReceiver();
            } else {
                // THE KEY NEVER SURVIVES A STOP (API 0.2): whoever asked -
                // the dome, a key binding, a browser, a remote session - a
                // stop opens any key and marks a latch's principal.
                api_->endKeyFromEngine("the receiver was stopped");
                // THE TEST SEAM OF FOXSDR_HOLD_AUDIO_LOCK, taken HERE so that the stop below runs while the audio mutex is held - for a control
                // request's stop only (the web remote's and a plugin's), as it always was.
                if (!commandFromWindow_ && holdAudioLockMs_ > 0 && !holdAudioLockDone_ && pipeline_.running()) { holdAudioLockForTest(); }
                stopReceiver(commandFromWindow_ ? "the window (the STOP dome, the Start/Stop key or the scope's POWER key)"
                                                : "a control request (the web remote or a plugin)");
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
            const double bwBefore = vfoBandwidthHz_;
            vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(c.num[0], bwHi));
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
            if (vfoBandwidthHz_ != bwBefore) { logBandwidthChange("remote request"); }
            if (vfoBandwidthHz_ != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            res.applied[0] = vfoBandwidthHz_;
            return res;
        }
        case FOXAPP_OP_SET_BANDWIDTH_STEP:
            // The rail's combo: one of the steps, exactly, never clamped.
            if (c.ival[0] < 0 || c.ival[0] >= kBwCount) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such bandwidth step");
            }
            setBandwidthIndex(static_cast<int>(c.ival[0]));
            res.applied[0] = vfoBandwidthHz_;
            return res;
        case FOXAPP_OP_BANDWIDTH_DRAG_END:
            // The drag has SETTLED (the window sends this when the mouse is let go): one line for the width it ended on, if it moved it.
            if (vfoBandwidthDragged_) {
                vfoBandwidthDragged_ = false;
                logBandwidthChange("dragged on the spectrum");
            }
            return res;
        case FOXAPP_OP_SET_BANDWIDTH_DRAG: {
            // A band edge dragged on the spectrum: clamped like SET_BANDWIDTH,
            // but the combo's step is left where it was (as it always was).
            const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
            const double dragBefore = vfoBandwidthHz_;
            vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(c.num[0], bwHi));
            pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
            if (vfoBandwidthHz_ != dragBefore) { vfoBandwidthDragged_ = true; }   // logged once, when the drag settles (BANDWIDTH_DRAG_END)
            if (vfoBandwidthHz_ != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            res.applied[0] = vfoBandwidthHz_;
            return res;
        }
        case FOXAPI_OP_SET_SQUELCH:
            squelchDb_ = static_cast<float>(c.num[0]);
            pipeline_.setSquelchDb(squelchDb_);
            // A HAND ON THE THRESHOLD IS A MANUAL CHOICE (stub campaign B12): the top bar's dial, the Radio section's slider, the keys, a web page - they all
            // send this op - and AUTOMATIC squelch goes off. (AUTOMATIC's own writes are not this op.)
            squelchAutoOff("a hand set the threshold");
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
        case FOXAPP_OP_SCAN_NATIVE_ONLY:
            // A Radio part added to the patch (0.99.40): it starts on a free
            // radio, so it needs the native list - which opens nothing - and
            // never the SoapySDR vendor probe (that is a Radio's own device
            // list opened, or "Look for radios").
            scanNative();
            return res;
        case FOXAPP_OP_PATCH_RADIO_LIST_OPENED:
            // A Radio's device list opening (0.99.40). The native walk runs
            // every time the list opens - it opens nothing, so nothing to
            // defer for. The SoapySDR probe is different: asked for ONCE
            // EVER (patchListsWanted_ guards it, exactly as
            // drawPatchRadioInspector did before the engine extraction), and
            // even then only as a WISH (patchScanWanted_) - patchReconcile
            // runs it on the first frame the scan plan can vouch for every
            // radio this process has open, never here, because a radio may
            // still be opening under this very call. Engine/stage3b-pre B2:
            // the branch used to reuse FOXAPP_OP_SCAN_DEVICES_ON_OPEN here,
            // which ran the probe IMMEDIATELY on every first open and never
            // retried a deferral - patchScanWanted_ was dead code as a
            // result (nothing ever set it).
            if (!patchListsWanted_) {
                patchListsWanted_ = true;
                if (!soapyScanned_ || soapyScanPartial_) { patchScanWanted_ = true; }
            }
            scanNative();
            return res;
        case FOXAPP_OP_PATCH_SET_GRAPH: {
            // THE PATCH PAGE'S EDIT, the whole graph at once (engine/stage3b-pre,
            // docs/engine-stage3.md OPEN 6, Design A). Read with the same
            // parse the config load trusts, ids kept; refused whole if any of
            // it cannot be honoured, so the graph the patch runs is always the
            // one the page drew or the one it had - never a repaired half.
            cascade::core::patch::Graph g;
            std::string why;
            if (!cascade::core::patch::graphFromCommandText(text, g, why)) {
                cascade::core::diagWarnf("patch: %s", why.c_str());
                return refuse(FOXAPI_BAD_ARGUMENT, why.c_str());
            }
            const bool document = (c.ival[0] & FOXAPP_PATCH_GRAPH_DOCUMENT) != 0;
            if (c.ival[1] < 0) { return refuse(FOXAPI_BAD_ARGUMENT, "a negative graph version"); }
            // THE VERSION THE SENDER EDITED FROM (API 0.3 8.12). Whatever the
            // pass has already changed in the graph - START's switches, a
            // centre the runtime learnt, an earlier session's edit in this very
            // pass - is noticed NOW, so the graph this command would replace is
            // the graph it is compared with. A draft whose base is not the
            // current version is MERGED over the current graph (what the
            // sender changed wins, everything else is the engine's), never put
            // over it: the sender cannot have seen what happened since.
            syncPatchGraphVersion();
            bool merged = false;
            const auto base = static_cast<std::uint64_t>(c.ival[1]);
            if (!document && base != 0 && base != patchGraphVersion_) {
                const PatchGraphRecord* from = nullptr;
                for (const PatchGraphRecord& r : patchGraphHistory_) {
                    if (r.version == base) { from = &r; }
                }
                if (from == nullptr) {
                    return refuse(FOXAPI_FAILED,
                                  "the patch graph moved on since this edit was started; it was not applied");
                }
                g = cascade::core::patch::rebaseDraft(from->graph, g, patchGraph_);
                merged = true;
            }
            // Ids are never handed out twice, across replacements too.
            g.reserveIds(patchGraph_.nextId());
            // No window hook: an interface that wants to know the graph moved
            // reads fact.patch.version (the same block as the graph and the
            // epoch), whoever moved it.
            patchGraph_ = std::move(g);
            if (document) {
                // A NEW PATCH: its own epoch, and no edit of the old one is
                // ever merged into it (the history goes; a sender still
                // holding an old version is refused "moved on").
                ++patchGraphEpoch_;
                ++patchGraphVersion_;
                patchGraphHistory_.clear();
                patchGraphHistory_.push_back({patchGraphVersion_, patchGraph_});
            } else {
                syncPatchGraphVersion();
            }
            res.applied[0] = static_cast<double>(patchGraphVersion_);
            res.applied[1] = merged ? 1.0 : 0.0;
            if (merged) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            return res;
        }
        case FOXAPP_OP_SET_PATCH_PAGE_OPEN:
            // THE STANDARD WINDOW'S PATCH VIEW IS SHOWING (API 0.3 8.12): the
            // engine's pass runs the patch runtime while it is (pumpPatch) and
            // stops the patch when it goes - the window's frame drives none
            // of it.
            patchPageOpen_ = on;
            return res;
        case FOXAPP_OP_PATCH_LIST_RECORDINGS:
            // THE RADIO'S RECORDINGS, listed on the engine's own pass (API 0.3 8.12): the Radio part's device list
            // opening, or "Look for radios", asks. The files are read through the probe cache, a bounded number of
            // headers a listing; PATCH_DEVICES and RECORDINGS change as the answer.
            patchListRecordings();
            return res;
        case FOXAPP_OP_SOUND_CARDS_WANTED:
            // The sound card row is showing (OPEN 2): list the cards once,
            // on the worker - never again while a list is being taken or a
            // card is opening, and not at all once they are listed.
            if (!soundCardListed_ && !soundCardScanPending_ && !soundCardOpenPending_) { scanSoundCards(); }
            return res;
        case FOXAPP_OP_PATCH_LOOK_FOR_RADIOS:
            // "Look for radios" pressed (0.99.40, engine/stage3b-pre
            // fields-to-commands round 2): unconditional, unlike the combo-open
            // case above - the operator asked outright, so the SoapySDR
            // probe's own "asked once" gate is bypassed by the unconditional
            // FOXAPI_OP_SCAN_DEVICES the window submits right after this.
            patchListsWanted_ = true;
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
            // The radio coerces a rate it does not offer to one it does: what
            // landed is the answer, and the API says it moved (stage 4).
            if (res.applied[0] != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            return res;
        }
        case FOXAPI_OP_SET_GAIN: {
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            // A stage the radio does not list is NOT_FOUND (API.md 8), never
            // handed to the driver - but for an AIRSPY, whose list is its
            // gain MODE's, a stage of another mode is a mode switch (the web
            // remote naming "LNA" from Linearity mode), not an unknown name.
            static const char* const kAirspyStages[] = {"LNA", "MIXER", "VGA", "LINEARITY", "SENSITIVITY"};
            const bool airspyStage =
                asAirspyDevice() != nullptr &&
                std::any_of(std::begin(kAirspyStages), std::end(kAirspyStages),
                            [&](const char* n) { return text == n; });
            if (!airspyStage &&
                std::find(deviceGainNames_.begin(), deviceGainNames_.end(), text) == deviceGainNames_.end()) {
                return refuse(FOXAPI_NOT_FOUND, "no gain stage by that name");
            }
            if (!device_->setGainDb(text, c.num[0])) {
                sourceError_ = device_->lastError();
                return refuse(FOXAPI_FAILED, "the radio refused the gain");
            }
            // THE READBACK, not the request: every one of these radios
            // quantises, and a slider left on the request would be a lie.
            for (std::size_t i = 0; i < deviceGainNames_.size(); ++i) {
                if (deviceGainNames_[i] == text && i < deviceGainsDb_.size()) {
                    deviceGainsDb_[i] = static_cast<float>(device_->gainDb(text));
                    res.applied[0] = floatAsDecimal(deviceGainsDb_[i]);
                    // A gain somebody chose is remembered for this radio (0.99.75; not an Airspy's, not under auto gain).
                    rememberManualGain(i);
                }
            }
            if (res.applied[0] != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            // AN AIRSPY's gain list is its MODE's, and a name from another
            // mode switches to it - so the list itself may have changed, and
            // the memory with it.
            if (cascade::gui::asAirspy(device_) != nullptr) {
                refreshDeviceGainMirrors();
                airspyRememberOpen();
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
                } else {
                    airspyRememberOpen();  // a no-op for any radio but an Airspy
                }
                res.applied[0] = deviceGainsDb_[i];
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no such gain stage");
        }
        case FOXAPI_OP_SET_DEVICE_AGC:
            if (device_ == nullptr) { return refuse(FOXAPI_NO_DEVICE, "no radio is open"); }
            if (!deviceAgcSupported_) { return refuse(FOXAPI_UNSUPPORTED, "no automatic gain"); }
            // The box's own path (changeAutoGain): on acceptance the mirror follows and the radio's memory is written (0.99.73); on a
            // refusal the driver's words go to sourceError_.
            if (!changeAutoGain(on)) { return refuse(FOXAPI_FAILED, "the radio refused"); }
            if (cascade::gui::asAirspy(device_) != nullptr) {
                refreshDeviceGainMirrors();
                airspyRememberOpen();
            }
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
            //
            // API 0.3 (8.3/9): the radio the sender was ASKED about may ride in
            // the text. The key asked about radio A, the person said yes, and
            // by the time this runs B may be the open one: the answer was not
            // given for B, so nothing is switched. No text, no check - the
            // Source panel's box and every other sender are as they were.
            if (!text.empty() && text != biasKeyRadioNow()) {
                return refuse(FOXAPI_NOT_FOUND, "the radio changed");
            }
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
        case FOXAPP_OP_SOUND_CARD_FORM: {
            // THE SOUND CARD PANEL'S WHOLE FORM (engine/stage3b-pre fields-to-
            // commands round 2, docs/engine-stage3.md OPEN item 1): every
            // widget in drawSoundCardControls used to write soundCard_ in
            // place; each now builds the CURRENT form (a local copy read from
            // this same field) with its one changed value and applies the
            // whole thing here, in the same frame - so nothing else in the
            // panel (the rate list, the "Receives..." preview) reads a stale
            // value later in the same draw pass. ival[0] is a bitmask
            // (1 = IqStereo, 2 = channel right, 4 = swapIq, 8 = pickedFromList);
            // num[0] cardRateHz, num[1] iqCentreHz; text "device<0x1F>hostApi" -
            // decoded by the same header that encodes it (soundcard_panel.hpp).
            const cascade::source::SoundCardSettings form = cascade::gui::soundCardFormFromCommand(c, text);
            // THE FIELD MASK (API 0.3 8.10, ival[1]): 0 is the whole form, as before. A set bit names a field the
            // command CARRIES; the others keep the engine's own value - so a control that changed one field cannot
            // write back a stale copy of another a pass changed since it last looked (a worker's open result
            // sets the whole form).
            const std::int64_t mask = c.ival[1];
            if (mask < 0 || mask > FOXAPP_SOUND_CARD_FIELDS_ALL) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such sound card form field");
            }
            if (mask == 0 || mask == FOXAPP_SOUND_CARD_FIELDS_ALL) {
                soundCard_ = form;
                return res;
            }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_DEVICE) != 0) {
                soundCard_.device = form.device;
                soundCard_.hostApi = form.hostApi;
            }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_FORMAT) != 0) { soundCard_.format = form.format; }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_CHANNEL) != 0) { soundCard_.channel = form.channel; }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_SWAP_IQ) != 0) { soundCard_.swapIq = form.swapIq; }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_PICKED) != 0) { soundCard_.pickedFromList = form.pickedFromList; }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_RATE) != 0) { soundCard_.cardRateHz = form.cardRateHz; }
            if ((mask & FOXAPP_SOUND_CARD_FIELD_CENTRE) != 0) { soundCard_.iqCentreHz = form.iqCentreHz; }
            return res;
        }
        case FOXAPP_OP_SOUND_CARD_PICK: {
            // A CARD CHOSEN FROM THE LIST (API 0.3 8.10; the Source section's Device combo): ival[0] is the
            // SOUND_CARDS entry's id, index + 1. What the pick handler in the window did, now the engine's: the
            // card's name and host API go in and are marked as chosen from this list (so they name exactly this
            // entry, even beside an identical card), the "not connected" line is cleared, and the rate is kept when
            // the new card offers it in the form's format - otherwise the card's own default, otherwise the
            // highest it has.
            if (!soundCardListed_) { return refuse(FOXAPI_NOT_FOUND, "the sound cards have not been listed"); }
            if (c.ival[0] < 1 || c.ival[0] > static_cast<std::int64_t>(soundCardDevices_.size())) {
                return refuse(FOXAPI_NOT_FOUND, "no such sound card entry");
            }
            const cascade::source::SoundCardDevice& d = soundCardDevices_[static_cast<std::size_t>(c.ival[0] - 1)];
            soundCardMissing_.clear();
            soundCard_.device = d.name;
            soundCard_.hostApi = d.hostApi;
            soundCard_.pickedFromList = true;
            const std::vector<cascade::source::SoundCardRate> rates =
                cascade::source::soundCardRatesFor(d, soundCard_.format);
            const bool offered = std::any_of(rates.begin(), rates.end(), [&](const cascade::source::SoundCardRate& r) {
                return r.hz == soundCard_.cardRateHz;
            });
            if (!offered && !rates.empty()) {
                soundCard_.cardRateHz = rates.back().hz;
                for (const cascade::source::SoundCardRate& r : rates) {
                    if (r.hz == d.defaultRateHz) { soundCard_.cardRateHz = r.hz; }
                }
            }
            return res;
        }

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
            // The new bookmark's id, as the API's result carries it (stage 4).
            res.applied[0] = static_cast<double>(addBookmarkHere(text));
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
            // A group nothing is in is NOT_FOUND, and changes nothing (stage 4).
            bool any = false;
            for (const cascade::core::Bookmark& b : freqMgr_.list()) { any = any || b.group == text; }
            if (!any) { return refuse(FOXAPI_NOT_FOUND, "no bookmark is in that group"); }
            // THE MONITOR IS PLAYING THIS PRESET (0.99.66 review, stub campaign B6): with its group gone the next pass would cut the blocks
            // again from "every ticked row" of the WHOLE list - a different set of channels from the one the user chose - or, with none
            // ticked, stop it with no reason. It stops here instead, saying why, BEFORE the rows go (the heard time goes into them first).
            if ((airbandListening_ || airbandStartPending_) && airbandGroup_ == text) {
                airbandStop(tr("Stopped: the preset was removed."));
            }
            if (airbandGroup_ == text) { airbandGroup_.clear(); }
            const std::size_t n = freqMgr_.removeGroup(text);
            bookmarkImportNote_ = cascade::core::formatText(tr("Removed %zu from \"%s\""), n, text.c_str());
            saveBookmarks();
            res.applied[0] = static_cast<double>(n);
            return res;
        }
        case FOXAPP_OP_BOOKMARK_IMPORT_FILE:
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no file named"); }
            // THE READ IS A WORKER'S (stub campaign B19): the answer is that it is under way. What it found - rows, or the sentence of a file
            // that would not open - is the note and fact.bookmarks.import.* when the worker has answered, a pass or more later. (Stub campaign B8
            // answered FAILED here, from a read on the pass, and the window opened its "Import a frequency list" window on that answer; it opens
            // it on fact.bookmarks.import.ok "0" now.)
            importBookmarkFile(text);
            return res;
        case FOXAPP_OP_BOOKMARK_EXPORT: {
            // ival[0] 0: the ids of the bookmarks the section shows, by the long-text channel; 1: the name of a preset (a bookmark group), whose
            // rows go out as a CSV (stub campaign B19). The engine writes the file, ON A WORKER (see exportBookmarks): the answer is that it is
            // under way, and the note - or, for a preset, fact.bookmarks.export.* - says where it went, or that it could not.
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refuse(FOXAPI_OUT_OF_RANGE, "ival[0] is 0 (SDR# for ids) or 1 (a preset as CSV)"); }
            const bool preset = c.ival[0] == 1;
            const std::int32_t st = exportBookmarks(text, preset);
            if (st == FOXAPI_BAD_ARGUMENT) { return refuse(st, preset ? "no preset named" : "no bookmark ids named"); }
            if (st == FOXAPI_NOT_FOUND) { return refuse(st, preset ? "that preset has no rows" : "none of those bookmarks exists"); }
            if (st == FOXAPI_NO_CHANGE) { return refuse(st, "an export is already being written"); }
            if (st != FOXAPI_OK) { return refuse(st, "the file could not be written"); }
            return res;
        }

        // --- the scanner -------------------------------------------------------------------
        case FOXAPI_OP_SCANNER_RUN:
            if (on) {
                // The range travels with the command and is kept as the
                // scanner's stored range; configure() sanitizes it (swaps a
                // reversed range, floors the step) whoever sent it.
                scanStartMhz_ = c.num[0] / 1.0e6;
                scanStopMhz_ = c.num[1] / 1.0e6;
                scanStepKhz_ = c.num[2] / 1.0e3;
                // A RANGE SCAN leaves list mode (the range travelled with the command: it is what was asked), and one radio takes one plan -
                // the Airband monitor stands down (stub campaign B6).
                scanTicked_ = false;
                if (airbandListening_ || airbandStartPending_) { airbandStop(std::string()); }
                configureScanner();
                scanner_.start(clockS() * 1000.0);
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
            if (scanner_.active()) { configureScanner(); }
            return res;
        case FOXAPP_OP_SCANNER_RANGE:
            if ((c.ival[0] & 1) != 0) { scanStartMhz_ = c.num[0] / 1.0e6; }
            if ((c.ival[0] & 2) != 0) { scanStopMhz_ = c.num[1] / 1.0e6; }
            if ((c.ival[0] & 4) != 0) { scanStepKhz_ = c.num[2] / 1.0e3; }
            // Only the panel reconfigures a running scan; a browser's range
            // is stored for the next start, as it always was.
            if ((c.ival[0] & 8) != 0 && scanner_.active()) { configureScanner(); }
            return res;
        case FOXAPP_OP_SCANNER_LIST: {
            std::string why;
            const std::int32_t st = applyScannerListOp(c, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        // --- the Airband monitor and the frequency list's ticks (stub campaign B6, docs/api-0.3-additions.md 9.6) ----------------------
        case FOXAPP_OP_AIRBAND: {
            std::string why;
            const std::int32_t st = applyAirbandOp(c, text, res, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        // --- AUTOMATIC SQUELCH (stub campaign B12, docs/api-0.3-additions.md 9.9) -------------------------------------------------------
        case FOXAPP_OP_SET_SQUELCH_AUTO: {
            std::string why;
            const std::int32_t st = applySquelchAutoOp(c, res, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        case FOXAPP_OP_BOOKMARK_TICK: {
            std::string why;
            const std::int32_t st = applyBookmarkTick(c, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        case FOXAPP_OP_BOOKMARK_ADD_ROWS: {
            std::string why;
            const std::int32_t st = applyBookmarkAddRows(c, text, res, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        case FOXAPP_OP_BOOKMARK_GROUP: {
            std::string why;
            const std::int32_t st = applyBookmarkGroup(c, text, res, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        // --- the waterfall's markers (stub campaign B10, docs/api-0.3-additions.md 9.8) ---------------------------------------------
        case FOXAPP_OP_MARKER_DROP:
        case FOXAPP_OP_MARKER_REMOVE:
        case FOXAPP_OP_MARKER_CLEAR:
        case FOXAPP_OP_MARKER_NOTE: {
            std::string why;
            const std::int32_t st = applyMarkerOp(c, text, res, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }

        // --- decoders and plugins ------------------------------------------------------------
        case FOXAPI_OP_DECODER_START:
        case FOXAPI_OP_DECODER_STOP: {
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no plugin named"); }
            // A name that is neither installed nor recorded as stopped is
            // NOT_FOUND (API.md 9), not a stop recorded against a name
            // nothing carries (engine stage 4). A START of a name recorded
            // as stopped is kept: it is how a stop outlives its plugin's
            // removal and is cleared again.
            bool known = false;
            for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
                known = known || cascade::core::pluginKey(lp) == text;
            }
            if (!known && !(c.op == FOXAPI_OP_DECODER_START && pluginIsStopped(text))) {
                return refuse(FOXAPI_NOT_FOUND, "no plugin with that name is installed");
            }
            setPluginStopped(text, c.op == FOXAPI_OP_DECODER_STOP);
            return res;
        }
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
        case FOXAPP_OP_PLUGIN_RUN_MODE: {
            // ONE PLUGIN'S RUN STATE (0.99.73; the Fitted modules window's KEEP RUNNING tick): the plugin's instances are started or stopped
            // alone. A name no installed plugin carries and no run state records is NOT_FOUND, as DECODER_START's is; an AUTO for an id the
            // map holds is how a stale entry is cleared.
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no plugin named"); }
            if (c.ival[0] < 0 || c.ival[0] > 2) { return refuse(FOXAPI_OUT_OF_RANGE, "no such run state"); }
            bool known = pluginRun_.find(cascade::core::pluginRunId(text)) != pluginRun_.end();
            for (const cascade::core::LoadedPlugin& lp : pluginHost_.plugins()) {
                known = known || cascade::core::pluginKey(lp) == text;
            }
            if (!known) { return refuse(FOXAPI_NOT_FOUND, "no plugin with that name is installed"); }
            setPluginRun(text, c.ival[0] == 1 ? cascade::core::PluginRun::Always
                               : c.ival[0] == 2 ? cascade::core::PluginRun::Stopped
                                                : cascade::core::PluginRun::Auto);
            return res;
        }
        case FOXAPP_OP_PLUGIN_USE:
            // What the window shows of each plugin (the whole table; empty clears it). Applied here, at the head of the pass; the lifecycle
            // step of THIS pass reads it - the window's signal is one pass late by design, the wake is not later still.
            applyPluginUse(text);
            return res;
        case FOXAPP_OP_PLUGIN_AUTO_PRESET:
            maybeAutoPreset(text, c.ival[0] == 1 ? "window opened" : "started", /*pin=*/c.ival[0] != 1);
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
            if (c.ival[0] < 0 || !pluginApi_->pressCommand(text, static_cast<std::uint32_t>(c.ival[0]))) {
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
            //
            // THE ENTRY IS NAMED BY ITS CATALOGUE KEY (0.99.74, stub campaign B3): the id for an official plugin - so every caller that
            // ever named one is unchanged - and "<id>@<site label>" for a plugin from a site the user added.
            //
            // A PLUGIN FROM A SITE IS FITTED FROM THE DESKTOP STORE ONLY: its page, its notice and the tick under it are what stand between
            // a stranger's native code and this machine, and a request from a browser cannot have read any of them. So a REMOTE session -
            // the web page's own is one - is refused it, however it names it (by key, or by the bare id the page lists: an id is unique in
            // the merged list, so that names exactly one entry) and whatever acknowledgement it sends. The refusal is the same sentence
            // the web page's catalogue rows carry (fillStatusLists).
            for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
                const cascade::core::PluginCatalogEntry& ce = catalog_[static_cast<std::size_t>(i)];
                const bool byKey = cascade::core::PluginRepo::catalogueKey(ce.source, ce.id) == text;
                if (!byKey && !(commandFromRemote_ && ce.id == text)) { continue; }
                if (commandFromRemote_ && !ce.source.empty()) {
                    installError_ = kSitePluginDesktopOnly;
                    return refuse(FOXAPI_DENIED, "plugins from sites you added can only be fitted from the plugin store on this computer");
                }
                const std::string blocked = pluginInstallBlockedReason(i, on);
                if (!blocked.empty()) {
                    installError_ = blocked;
                    return refuse(FOXAPI_DENIED, "the install was refused");
                }
                startInstall(ce);
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
                if (cascade::core::PluginRepo::catalogueKey(u.source, u.id) != text) { continue; }
                startUpdate(u);
                return res;
            }
            return refuse(FOXAPI_NOT_FOUND, "no update planned for that plugin");
        }
        // --- the store's sites, pictures and clean-up (stub campaign B3, docs/api-0.3-additions.md 9.4) ------------------------------
        case FOXAPP_OP_STORE_SITE: {
            siteError_.clear();
            if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) {
                return refuse(FOXAPI_BUSY, "a transfer is already in progress");
            }
            if (c.ival[0] == 1) {
                // REMOVE. A site that is not listed changes nothing.
                if (std::find(pluginSites_.begin(), pluginSites_.end(), text) == pluginSites_.end()) {
                    return refuse(FOXAPI_NOT_FOUND, "no such site in the list");
                }
                removePluginSite(text);
                return res;
            }
            if (c.ival[0] != 0) { return refuse(FOXAPI_OUT_OF_RANGE, "ival[0] is 0 (add) or 1 (remove)"); }
            // ADD, only with the consent: the box that says what a site is (docs/THIRD-PARTY-SITES.md) was ticked. Nothing is read or written
            // without it.
            if (c.ival[1] == 0) {
                siteError_ = "the box that says what a plugin site is was not ticked";
                return refuse(FOXAPI_DENIED, siteError_.c_str());
            }
            std::string why;
            if (!addPluginSite(text, why)) {
                siteError_ = why;
                return refuse(FOXAPI_BAD_ARGUMENT, why.c_str());
            }
            return res;
        }
        case FOXAPP_OP_STORE_PICTURES_WANT:
            if (catalogIndexByKey(text) < 0) { return refuse(FOXAPI_NOT_FOUND, "no catalogue entry with that key"); }
            requestScreenshots(text, on);
            return res;
        case FOXAPP_OP_PLUGIN_SETTING_EDIT: {
            // A PLUGIN'S SETTINGS FORM, edited (stub campaign B9; engine_plugin_settings.cpp): the verdict is the answer's applied[0], whether or not it
            // was accepted - the window snaps a refused box back from it.
            double verdict = 0.0;
            std::string why;
            const std::int32_t st = applyPluginSettingEdit(text, verdict, why);
            res.applied[0] = verdict;
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            return res;
        }
        case FOXAPP_OP_STORE_CLEAN_OLD:
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refuse(FOXAPI_OUT_OF_RANGE, "ival[0] is 0 (clean up) or 1 (remove one orphaned file)"); }
            if (catalogPending_ || installPending_ || screenshotPending_ || cleanupPending_) {
                return refuse(FOXAPI_BUSY, "a transfer is already in progress");
            }
            if (c.ival[0] == 0) {
                cleanUpOldVersionsConfirmed();
            } else {
                removeOrphanedPlugin(text);
            }
            return res;
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
            // The API's form: no per-module tick to carry.
            startAddAll(on);
            return res;
        case FOXAPP_OP_STORE_ADD_ALL:
            // The store window's key: the ADD ALL tick, and the one module
            // whose own notice tick is on (OPEN 5).
            // ival[1] 1 (stub campaign B4): the UPDATE ALL key - the same queue, updates only.
            startAddAll(on, text, c.ival[1] != 0);
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
            // AN ADDRESS OR NOTHING (round 3 fix): an empty id used to keep
            // whatever transmitArgs_ held and open THAT, so clearing the
            // Transmit box and pressing Open opened the previous board - one
            // the box no longer showed, on the page whose whole point is
            // knowing what is about to radiate.
            if (text.find_first_not_of(" \t") == std::string::npos) {
                return refuse(FOXAPI_BAD_ARGUMENT, "no transmitter address given");
            }
            // A KEY NEVER SURVIVES A TRANSMITTER CHANGE (API 0.2).
            api_->endKeyFromEngine("the transmitter was changed");
            transmitArgs_ = text;
            openTransmitRadio();
            if (!transmitter_.haveSink()) { return refuse(FOXAPI_FAILED, "the transmitter did not open"); }
            return res;
        case FOXAPI_OP_TX_CLOSE:
            api_->endKeyFromEngine("the transmitter was closed");
            closeTransmitRadio();
            return res;
        case FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN:
            // THE PAGE OPEN FLAG (engine/stage3b-pre 2b): which page the
            // window shows. Since stage 4 it gates NO key - the remote key
            // needs the local operator's explicit consent (TX_REMOTE_ARM from
            // the window's session, the page's "Allow remote transmit"
            // switch), and the window withdraws it, and releases its own key,
            // when the page closes.
            transmitOpen_ = on;
            if (on && transmitArgs_.empty()) {
                // THE ADDRESS IS SEEDED FOR THE EYE (engine/stage3b-pre
                // fields-to-commands round 2, moved from the window's own
                // per-frame seed): doing it only inside the open left the
                // field BLANK on the page, so the panel offered no answer at
                // all to "which board would this be?" until after somebody
                // had already pressed Open.
                transmitArgs_ = transmitArgsSeed();
            }
            return res;
        case FOXAPI_OP_TX_PTT:
            // REFUSED, NOT HANDLED (stage 5d). The key is decided by the key rules for a SESSION (core/tx_key_rules.hpp), in that session's
            // place in the table's queues - ApiHostImpl::apply, which never calls this function for it. The browser's PTT used to arrive
            // here (the web remote's private fake session, ApiHost::webRemotePtt); it is the web server's engine session's now, like any
            // other client's, and nothing that reaches applyCommand can speak for a session: a TX_PTT here has no sender to judge, no
            // consent to check and no hold to buy, so it is refused rather than guessed at. (Kept as a refusal and not deleted so that a
            // caller that has not heard is told, not left to the "no such operation" of the default arm.)
            return refuse(FOXAPI_DENIED, "the transmit key is pressed through a session, not applied directly");
        case FOXAPP_OP_TX_PLUGIN_ARM:
            // REFUSED, NOT HANDLED, as a PTT is: the arming window belongs to a SESSION (its lease: ApiHostImpl::apply calls txPluginArmOp for the sender it knows), and nothing that
            // reaches applyCommand speaks for one (stub campaign B13).
            return refuse(FOXAPI_DENIED, "the arming window is opened through a session, not applied directly");
        case FOXAPP_OP_TX_DATA_FLAT:
            if (commandFromRemote_) { return refuse(FOXAPI_DENIED, "the application's own commands are for sessions on this machine"); }
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refuse(FOXAPI_OUT_OF_RANGE, "0 or 1"); }
            txDataFlat_ = on;
            transmitter_.setDataFlat(txDataFlat_);
            res.applied[0] = txDataFlat_ ? 1.0 : 0.0;
            return res;
        case FOXAPP_OP_PLUGIN_TX_GRANT: {
            // THE TRANSMIT GRANT (stub campaign B13): LOCAL sessions only. The table refuses a REMOTE sender three ways at submit; this is the fourth, where it is applied, for any
            // path that reaches here with the remote flag set (ApiHostImpl::apply sets it around every call).
            if (commandFromRemote_) { return refuse(FOXAPI_DENIED, "the application's own commands are for sessions on this machine"); }
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refuse(FOXAPI_OUT_OF_RANGE, "0 revokes, 1 grants"); }
            std::string why;
            const std::int32_t st = applyPluginTxGrantOp(text, on, why);
            if (st != FOXAPI_OK) { return refuse(st, why.c_str()); }
            res.applied[0] = on ? 1.0 : 0.0;
            return res;
        }
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
            if (c.ival[0] < 0 || c.ival[0] >= cascade::core::kTxHandInputCount) {
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
        case FOXAPI_OP_SCAN_SERIAL_PORTS:
            // API 0.3: the ports of this machine, listed on the engine's
            // thread when asked (the window's GPS drop-down, the Serial
            // ports page) - FOXAPI_LIST_SERIAL_PORTS, listSeq moving.
            serialPorts_ = testHooks_.serialPorts != nullptr ? testHooks_.serialPorts()
                                                            : cascade::core::enumerateSerialPorts();
            ++serialPortsScans_;
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
        case FOXAPI_OP_TELEMETRY_ENABLE:
            // REFUSED, FROM EVERY ORIGIN (round 3 fix, privacy). Usage
            // reporting is consent given at this machine; the desktop's own
            // checkbox sends FOXAPP_OP_TELEMETRY_CONSENT, which no transport
            // carries. Implemented only as this refusal, so that once an API
            // session forwards FOXAPI ops no client can switch reporting on
            // silently - on or off, the answer is the same.
            return refuse(FOXAPI_DENIED, "usage reporting is switched only at the desktop");
        case FOXAPP_OP_TELEMETRY_CONSENT:
            // engine/stage3b-pre "next round" item 1: was two direct field
            // writes plus a direct call to telemetryHeartbeat_.configure(...)
            // from AppWindow::drawUsageReportingSection's Checkbox. THE ID IS
            // MINTED AT THE MOMENT OF CONSENT, NEVER BEFORE - a machine that
            // never opts in has no identifier at all - and OFF DELETES IT, so
            // a later opt-in cannot be tied to the old one. ON WHILE ALREADY
            // ON changes nothing (the checkbox's own `on && !enabled`): the
            // id a machine reports under must not change behind its back.
            if (on) {
                if (telemetryEnabled_) { return refuse(FOXAPI_NO_CHANGE, "usage reporting is already on"); }
                telemetryInstallId_ = cascade::core::newInstallId();
                telemetryEnabled_ = !telemetryInstallId_.empty();
            } else {
                telemetryEnabled_ = false;
                telemetryInstallId_.clear();
            }
            // THE HEARTBEAT FOLLOWS THE SWITCH IN THE SAME STEP: off disarms
            // it (configure refuses the now-empty id), on arms it with the
            // new id - exactly as the window's click handler always did.
            telemetryHeartbeat_.configure(cascade::core::telemetryEndpoint(), telemetryInstallId_,
                                          cascade::versionString());
            return res;
        case FOXAPI_OP_SERVER_CONFIG: {
            // Stage 5a (docs/api-0.3-additions.md 8.16): the local operator's configuration of the web server and the CAT listener.
            // LOCAL + ADMIN only (validate), the JSON in the text (the first 255 bytes are all validate sees, so a longer one
            // comes by submit_text), strict (ServerConfigRequest). The configuration is the engine's and rides the config file; the
            // listeners it describes are the engine's own since 5b: this records and publishes, and the host's WORKER makes them match
            // (start, restart, stop - never in the pass). The password is hashed on a worker the pass polls (a click used to run PBKDF2
            // on the GUI thread).
            ServerConfigRequest req;
            std::string why;
            if (!parseServerConfigRequest(text, req, why)) { return refuse(FOXAPI_BAD_ARGUMENT, why.c_str()); }
            const ServerHost::Outcome o = servers_.apply(req);
            res.status = o.status;
            if (!o.message.empty()) { std::snprintf(res.message, sizeof(res.message), "%s", o.message.c_str()); }
            return res;
        }
        case FOXAPP_OP_TRACK_INFO_LINE: {
            // The track-info plugin's status ("cannot reach the registry") lands in the same log as the decoders' lines: it is
            // the only place a user looks when a plugin is quiet. SENT BY THE WINDOW (it owns the plugin's cache and polls its
            // text on its own thread - TrackInfoCache is GUI-thread only); the pump move's step P2 replaced the host hook this
            // pass used to ask (drainTrackInfoText) with this command.
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no line"); }
            cascade::core::DecodedLine l;
            l.plugin = "Aircraft info";
            l.text = text;
            // COUNTED like every other line (API 0.3 8.7): an uncounted one never reached the backlog an interface reads (the
            // ring takes the lines the count says are new), so the window that draws the Decoder output window from that
            // backlog would have lost the only place a user sees why the track-info plugin is quiet.
            ++decoderLinesTotal_;
            decoderLog_.push_back(std::move(l));
            while (decoderLog_.size() > kDecoderLogMax) { decoderLog_.pop_front(); }
            return res;
        }
        case FOXAPP_OP_NOTE_PANEL:
            // Window port group 3: the page names itself ONCE a run (the
            // window's own once-per-name set), where it used to call
            // telemetryNotePanel from its frame every frame it was open. The
            // list is the usage report's `panels`: de-duplicated and capped at
            // 16 by telemetryNotePanel, so a repeat or a seventeenth name is
            // an answered no-op, not a refusal. A REMOTE session never gets
            // here (every app op is refused it, validate): its pages are not
            // this installation's usage.
            if (text.empty()) { return refuse(FOXAPI_BAD_ARGUMENT, "no panel named"); }
            telemetryNotePanel(text.c_str());
            return res;

        // --- status lines and flags a panel used to clear or consume in place
        // (engine/stage3b-pre 2c, docs/engine-stage3.md OPEN 1) --------------
        case FOXAPP_OP_SET_SOURCE_ERROR:
            // The window's OWN validation (a typed frequency it could not
            // parse) - text empty clears it, exactly as `.clear()` did.
            sourceError_ = text;
            return res;
        case FOXAPP_OP_SET_BOOKMARK_NOTE:
            bookmarkImportNote_ = text;
            return res;
        case FOXAPP_OP_SET_PLUTO_URI:
            // The Source section's Pluto address box, committed as it is
            // typed (round 3 fix) so an address typed and never opened is
            // still saved - as it was when the box wrote the field in place.
            // openPlutoAt stores the address it opens as well.
            cascade::core::formatUtf8(plutoUri_, sizeof(plutoUri_), "%s", text.c_str());
            return res;
        case FOXAPP_OP_SET_AUDIO_BUFFER:
            // The Sinks rail's AUDIO BUFFER combo (0.99.73): 0 AUTOMATIC, 120, 240, 480 or 960 ms.
            if (c.ival[0] < 0 || c.ival[0] > 100000 || !cascade::sink::validAudioBufferSetting(static_cast<int>(c.ival[0]))) {
                return refuse(FOXAPI_OUT_OF_RANGE, "no such buffer");
            }
            setAudioBufferMs(static_cast<int>(c.ival[0]));
            return res;
        case FOXAPP_OP_TEST_HOLD_AUDIO_LOCK:
            // FOXSDR_HOLD_AUDIO_LOCK (bounded runs only; the window reads the variable and sends it): see holdAudioLockMs_.
            holdAudioLockMs_ = static_cast<int>(c.ival[0]);
            return res;
        case FOXAPP_OP_SET_PPM:
            // The Source section's PPM controls (0.99.56): ival[0] the switch, num[0] this radio's value (sanitised, 0 = none).
            changePpm(c.ival[0] != 0, c.num[0]);
            return res;
        case FOXAPP_OP_SET_RTLTCP_ADDR:
            // The rtl_tcp address box, committed as it is typed - the Pluto box's twin (openRtlTcpAt stores the address it opens as well).
            cascade::core::formatUtf8(rtlTcpAddr_, sizeof(rtlTcpAddr_), "%s", text.c_str());
            return res;
        case FOXAPP_OP_SET_TRANSMIT_ARGS:
            // The Transmit page's address box, the same way. Stored only:
            // nothing opens until TX_OPEN, and a board already open keeps
            // the address it was opened with (the box is disabled then).
            if (transmitter_.haveSink()) { return refuse(FOXAPI_FAILED, "the transmitter is open"); }
            transmitArgs_ = text;
            return res;
        case FOXAPP_OP_SET_CATALOGUE_URL:
            // Committed on deactivate-after-edit (the window's pluginUrlBuf_,
            // never per keystroke) - unchanged, just applied through the
            // command path now.
            pluginCatalogueUrl_ = text;
            return res;
        case FOXAPP_OP_CLEAR_STATUS:
            switch (c.ival[0]) {
                case cmd::FOXAPP_STATUS_GPS_REFUSAL:
                    gpsRefusal_.clear();
                    return res;
                case cmd::FOXAPP_STATUS_SOUND_CARD_MISSING:
                    soundCardMissing_.clear();
                    return res;
                case cmd::FOXAPP_STATUS_DECODER_LOG:
                    decoderLog_.clear();
                    return res;
                case cmd::FOXAPP_STATUS_SOAPY_SCAN_DEFERRED_LOGGED:
                    soapyScanDeferredLogged_ = false;
                    return res;
                case cmd::FOXAPP_STATUS_MUTE_POPUP_QUEUED:
                    mutePopupQueued_ = false;
                    return res;
                case cmd::FOXAPP_STATUS_GPS_RESULT:
                    // The finished read's line (GPS list 48, API 0.3 8.13): the window sends this with GPS_REFUSAL when
                    // the port or the baud is edited. A no-op while a read is running, as the reader's own call is.
                    gpsReader_.clearResult();
                    return res;
                default:
                    return refuse(FOXAPI_OUT_OF_RANGE, "no such status line");
            }
        case FOXAPP_OP_MUTE_KEEP_RUNNING:
            // "Keep it running" (engine/stage3b-pre fields-to-commands round
            // 2): KEEPS THE MUTE, deliberately - releasing the audio here
            // would make "keep it running" mean "and also undo the muting",
            // a different answer to a question nobody asked. Both fields move
            // together, in the same command, exactly as the direct writes
            // did: the question has been answered, so clearing the subject is
            // what makes the NEXT one a fresh capture rather than a second
            // showing of this one.
            muteKeptRunning_ = true;
            mutePopup_ = cascade::core::MutePopupSubject{};
            return res;

        // --- the Airspy R2/Mini panel (0.99.41, engine/stage3b-pre Airspy
        // round, docs/engine-stage3.md OPEN 2/3) -----------------------------
        case FOXAPP_OP_AIRSPY_DECIMATION:
            if (!chooseAirspyDecimation(static_cast<unsigned>(c.ival[0]))) {
                return refuse(FOXAPI_FAILED, "the radio refused that decimation");
            }
            return res;
        case FOXAPP_OP_AIRSPY_GAIN_MODE:
            if (c.ival[0] < 0 || c.ival[0] > 2) { return refuse(FOXAPI_OUT_OF_RANGE, "no such gain mode"); }
            if (!chooseAirspyGainMode(static_cast<cascade::source::AirspySource::GainMode>(c.ival[0]))) {
                return refuse(FOXAPI_FAILED, "the radio refused that gain mode");
            }
            return res;
        case FOXAPP_OP_AIRSPY_AGC:
            if (c.ival[0] != 0 && c.ival[0] != 1) { return refuse(FOXAPI_OUT_OF_RANGE, "no such AGC stage"); }
            if (!chooseAirspyAgc(c.ival[0] == 0, c.ival[1] != 0)) {
                return refuse(FOXAPI_FAILED, "the radio refused that AGC switch");
            }
            return res;

        default:
            break;
    }
    return refuse(FOXAPI_UNSUPPORTED, "this build has no such operation");
}

void Engine::telemetryAccrueMode() {
    // Seconds are banked against the mode that was ACTUALLY running, once per
    // frame. Accumulating against the mode selected at shutdown would credit
    // the whole session to whatever happened to be last.
    //
    // The accrual carries the sub-second remainder between calls. It has to:
    // a frame is ~17 ms, so every individual delta truncates to zero seconds
    // and nothing would ever be banked.
    const std::uint64_t secs = telemetryModeAccrual_.advance(host_->wallTimeS());
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

void Engine::telemetryStartup(const cascade::core::AppConfig& cfg, const std::string& configDir) {
    telemetryEnabled_ = cfg.telemetryEnabled;
    telemetryInstallId_ = cfg.telemetryInstallId;
    telemetryFirstRun_ = cfg.telemetryFirstRun;
    telemetryFirstVersion_ = cfg.telemetryFirstVersion;
    // Reporting is on by default, so a first run arrives here enabled with no
    // identifier. Mint one now. If the CSPRNG fails there is no id, and
    // reporting stays off rather than falling back to anything guessable.
    if (telemetryEnabled_ && telemetryInstallId_.empty()) {
        telemetryInstallId_ = cascade::core::newInstallId();
        telemetryEnabled_ = !telemetryInstallId_.empty();
        // The first run is NOW - and only now: an id that already existed
        // before these fields did keeps them empty rather than claiming
        // today as its first day.
        if (telemetryEnabled_) {
            telemetryFirstRun_ = cascade::core::utcDateToday();
            telemetryFirstVersion_ = cascade::versionString();
        }
    }
    telemetryLaunches_ = cfg.telemetryLaunches + 1;
    telemetryCrashes_ = cfg.telemetryCrashes;
    telemetryExits_.died = cfg.telemetryExitsDied;
    telemetryExits_.killed = cfg.telemetryExitsKilled;
    telemetryExits_.ended = cfg.telemetryExitsEnded;
    telemetryExits_.unknown = cfg.telemetryExitsUnknown;
    // WHEN THIS SESSION BEGAN, for the next start-up (core/exit_cause.hpp, "which
    // report belongs to the previous session"). Written by every save of this
    // session, the first of which is the forced one at the top of the frame loop that
    // also puts the unclean-exit marker on disk, so the two reach the file together.
    {
        const std::time_t began = std::time(nullptr);
        telemetrySessionStartedEpoch_ = began > 0 ? static_cast<std::uint64_t>(began) : 0u;
    }
    // The previous run never wrote its clean-exit marker, so it did not end
    // normally. This is the whole crash-counting mechanism: no crash handler,
    // no minidump, nothing uploaded from the failure itself - just the
    // observation that last time the marker was never set.
    if (!cfg.telemetryCleanExit) {
        ++telemetryCrashes_;
        // HOW IT ENDED (0.99.69). Counted as `unknown` in the same breath as
        // `crashes` goes up, so the four classes add up to the unclean exits at every
        // instant; the evidence is read a moment later, off this thread, and moves it
        // to its class (exitCausePoll). A run with nothing to read - Diagnostics off,
        // or a predecessor that recorded no start - is final at once.
        telemetryExits_.noteUnclean();
        exitCausePrevStart_ = cfg.telemetrySessionStarted;
        if (!cfg.diagnosticsEnabled) {
            exitCauseFinish(cascade::core::unknownExit(
                "Diagnostics were off, so nothing was recorded about it"));
        } else if (exitCausePrevStart_ == 0) {
            exitCauseFinish(cascade::core::classifyPreviousExit(
                cascade::core::ReportListing{}, 0, static_cast<std::int64_t>(telemetrySessionStartedEpoch_)));
        } else {
            exitCausePending_ = true;
            // The read of the previous session's reports starts now, on a worker: the evidence is in the files, and this session's own
            // lines can rotate them (the window's bundle reads through a cache of its own).
            const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count();
            if (!cascade::core::diagCrashDir().empty()) {
                exitHistory_.refresh(cascade::core::diagLogDir(), cascade::core::diagCrashDir(), nowMs, 0);
            }
        }
    }
    // (The same marker is also the window's trigger to offer a crash report,
    // and the crash-loop limiter's memory is read beside it: both are the
    // window's diagnostics, AppWindow::diagnosticsStartup.)
    telemetrySessionStart_ = host_->wallTimeS();
    telemetryModeAccrual_.reset(telemetrySessionStart_);
    // DISPLAY STALLS. Armed only for a real identity, which an opted-out run
    // does not have - so off means nothing is counted and nothing is kept - and
    // it picks up what earlier sessions left (a session ended from the taskbar
    // mid-freeze left its count in this file, written by the watchdog's own
    // thread). This is start-up: the read is not on the frame path. An opted-out
    // run also clears a file an earlier opted-in run left behind.
    telemetryLedgerPath_ = cascade::core::StallLedger::pathIn(configDir);
    if (telemetryEnabled_ && !telemetryInstallId_.empty()) {
        stallLedger_->arm(telemetryLedgerPath_, telemetryInstallId_, /*loadExisting=*/true);
    } else {
        stallLedger_->disarm();
        cascade::core::StallLedger::removeFile(telemetryLedgerPath_);
    }
    // THE FAILURES THAT ARE NOT CRASHES (0.99.64), on the same terms: armed only
    // for a real identity, which an opted-out run does not have, and counted only
    // while Diagnostics is on as well (the rule the stall count follows). It
    // picks up what earlier sessions left AND keeps what THIS run counted before
    // it knew whether reporting was on - the saved radio is opened by the config
    // restore, which ran first, and that failure belongs to this session. An
    // opted-out run drops all of it and removes a file an earlier opted-in run
    // left.
    healthLedgerPath_ = cascade::core::health::HealthLedger::pathIn(configDir);
    healthLedger_->setAllowed(cfg.diagnosticsEnabled);
    if (telemetryEnabled_ && !telemetryInstallId_.empty()) {
        healthLedger_->arm(healthLedgerPath_, telemetryInstallId_, /*loadExisting=*/true);
    } else {
        healthLedger_->disarm();
        cascade::core::health::HealthLedger::removeFile(healthLedgerPath_);
    }
    // Last session's report goes now, on a thread, while the window is coming
    // up. Nothing waits for it and nothing reports if it fails.
    //
    // ONCE. The report stays in config.json until the first save of this
    // session replaces it, so a launch that dies before that save - or a
    // second copy started while this one is still coming up - would send it
    // again; one install sent the same report fifteen times in one second.
    // claimReportSend lets exactly one process send each report (telemetry.hpp).
    // A hermetic run (no config path) has no directory to claim in and keeps
    // the old behaviour.
    if (telemetryEnabled_ && !telemetryInstallId_.empty() &&
        !cfg.telemetryPending.empty()) {
        // prepareStartupRecord claims the report AS STORED and hands back what
        // to send: the record with the ledger's count as of THIS moment - the
        // ledger is written by the watchdog thread the instant a stall is
        // classified, so it holds the freeze the user ended from the taskbar,
        // which no save ever saw. The ledger is reduced by exactly `carried`,
        // and only if the server answers 2xx.
        std::string record;
        std::uint64_t carried = 0;
        // The health counts ride the same record, and are taken off by the same
        // acceptance: what EARLIER sessions left (this run's own counts describe
        // this session and go with the next record).
        cascade::core::health::Counts carriedHealth;
        if (cascade::core::prepareStartupRecord(configDir, cfg.telemetryPending, *stallLedger_,
                                                *healthLedger_, record, carried, carriedHealth)) {
            telemetryReporter_.send(
                cascade::core::telemetryEndpoint(), record,
                cascade::core::settleOnAccept(stallLedger_, carried, healthLedger_,
                                              std::move(carriedHealth)));
        }
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
    cfg.telemetryFirstRun = telemetryFirstRun_;
    cfg.telemetryFirstVersion = telemetryFirstVersion_;
    cfg.telemetryLaunches = telemetryLaunches_;
    cfg.telemetryCrashes = telemetryCrashes_;
    cfg.telemetryExitsDied = telemetryExits_.died;
    cfg.telemetryExitsKilled = telemetryExits_.killed;
    cfg.telemetryExitsEnded = telemetryExits_.ended;
    cfg.telemetryExitsUnknown = telemetryExits_.unknown;
    cfg.telemetrySessionStarted = telemetrySessionStartedEpoch_;
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
    r.channel = cascade::core::installChannel();
    r.firstRun = telemetryFirstRun_;
    r.firstVersion = telemetryFirstVersion_;
    r.launches = telemetryLaunches_;
    r.crashes = telemetryCrashes_;
    r.exits = telemetryExits_;
    // One atomic read - the frame path never touches the ledger's file. This is
    // the figure as of THIS save; what is actually sent is the ledger's own at
    // the next start-up (see telemetryStartup), which also holds a stall that
    // happened after the last save.
    r.stalls = stallLedger_->count();
    // The health counts as of THIS save, for the same reason: what is actually
    // sent is the ledger's at the next start-up (telemetryStartup).
    r.health = healthLedger_->encoded();
    const double now = host_->wallTimeS();
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

void Engine::healthPoll() {
    // THE OUTPUT HAS PLAYED, once a session: the sink's own latch, set by the
    // audio callback when the ring held its lead and the first real block went
    // out (AudioOut::primed). One relaxed atomic read a pass until it is seen.
    // The host API is asked for with it, and a pass on which the sink is busy
    // being (re)opened answers empty - the next pass asks again.
    if (!healthSoundNoted_ && pipeline_.audio().primed()) {
        const std::string api = pipeline_.audio().openedHostApi();
        if (!api.empty()) {
            healthSoundNoted_ = true;
            cascade::core::health::noteSoundOk(api);
        }
    }
    // AN OPENED RADIO'S SAMPLES HAVE ARRIVED: a spectrum frame newer than the
    // newest one the pipeline held when the radio was installed can only have
    // been made from this radio's samples (installing a source quiesces and
    // joins the old one's threads first). Read with the watch's own cursor, so
    // the display's is untouched, and only until the radio has been seen.
    if (healthWatching_ && pipeline_.getLatestFrame(healthProbe_) &&
        healthProbe_.seq > healthWatchSeq_) {
        healthWatching_ = false;
        cascade::core::health::noteRadioData(healthWatchKind_);
    }
    // WHAT THE PROGRAM RECOVERED FROM IN PLACES THIS ENGINE MUST NOT COUNT FROM
    // (0.99.65): the flags a plugin's thread raised, and four counters the
    // pipeline's threads and a vendor call's worker keep. Every one is read here,
    // by a poll the pass already makes - nothing is counted on the DSP,
    // source or audio thread, and the cost is one atomic exchange and four relaxed
    // loads a pass.
    cascade::core::health::drainRecovered();
    cascade::core::health::RecoveryReadings now;
    now.sourceThreadsAbandoned = static_cast<unsigned long long>(pipeline_.abandonedSourceThreads());
    now.vendorCallsAbandoned = cascade::source::SoapySource::driverCallsAbandoned();
    now.ringDroppedSamples = pipeline_.ringDroppedSamples();
    now.dspThreadExceptions = static_cast<unsigned long long>(pipeline_.dspThreadExceptions());
    recoveryWatch_.poll(now);
}

void Engine::healthWatchSamples(const std::string& kind) {
    healthWatchKind_ = kind;
    healthProbe_ = cascade::core::SpectrumFrame{};
    // The newest frame the pipeline holds right now, if any, is the baseline.
    healthWatchSeq_ = pipeline_.getLatestFrame(healthProbe_) ? healthProbe_.seq : 0;
    healthWatching_ = true;
}

void Engine::exitCausePoll() {
    if (!exitCausePending_) { return; }
    // Nothing to read from, now: the switch was turned off, or this run may not
    // touch the disk at all (a bounded run with no reports folder). Final.
    if (!healthLedger_->allowed() || cascade::core::diagCrashDir().empty()) {
        exitCauseFinish(cascade::core::unknownExit(
            healthLedger_->allowed() ? "this run may not read the reports folder"
                                     : "Diagnostics were switched off before it could be read"));
        return;
    }
    // The listing is read on a worker (telemetryStartup); until it has answered,
    // keep waiting - a mutex test, no disk.
    if (!exitHistory_.ready()) { return; }
    const cascade::core::DiagHistory h = exitHistory_.snapshot();
    if (h.pending) { return; }
    exitCauseFinish(cascade::core::classifyPreviousExit(
        h.reports, static_cast<std::int64_t>(exitCausePrevStart_),
        static_cast<std::int64_t>(telemetrySessionStartedEpoch_)));
}

void Engine::exitCauseFinish(const cascade::core::ExitVerdict& v) {
    exitCausePending_ = false;
    // One line, whatever the answer, so a log read later says how the session
    // before it ended and why that is the answer ("unknown" says why too).
    cascade::core::diagLogf("%s", cascade::core::previousExitLogLine(v).c_str());
    // The count is moved from `unknown` to the class; for `unknown` itself it is
    // already there. Either way the total has not changed.
    telemetryExits_.refine(v.cls);
}

// THE RECEIVER'S HALF OF THE CONFIG RESTORE (engine stage 3a): every line of
// AppWindow::applyConfig that set the receiver, in the order it always ran;
// the window keeps its view state (docs/engine-stage3.md). `cfg` is the
// start-up state (core::startupState), not the file as saved.
void Engine::applyConfig(const cascade::core::AppConfig& cfg) {
    loadedCfg_ = cfg;  // see loadedCfg_ in engine.hpp
    servers_.load(cfg);  // the web server's and the CAT listener's half (stage 5a)
    // The device scan's UHD rule (gui::soapyDriversWithNoHardware): the switch,
    // and the Soapy args the file named whether or not that radio opens this
    // launch - a USRP that is switched off or unreachable today is still one
    // the user has.
    lookForNetworkUsrps_ = cfg.lookForNetworkUsrps;
    startupSoapyArgs_ = cfg.soapyArgs;
    // Panel mirrors + always-safe DSP settings first (none of these can
    // fail; load() already range-sanitized volume/split/db*).
    volume_ = cfg.volume;
    pipeline_.audio().setVolume(volume_);
    dbMin_ = cfg.dbMin;
    dbMax_ = cfg.dbMax;
    // The audio buffer (0.99.73): a fixed value is applied at once, before the
    // first prime has had time to matter; AUTOMATIC leaves the 120 ms start.
    setAudioBufferMs(cfg.audioBufferMs);
    // THE DEPTH AUTOMATIC REACHED LAST TIME (audioBufferAutoMs): an AUTOMATIC session starts there
    // - never below the 120 ms it would start at, never above 960 - so a slow computer does not
    // pay the same bad minutes at every launch. A fixed setting ignores it (and keeps it for the
    // file). It only ever starts DEEPER: a restore on an engine whose session has already gone
    // deeper than the file says leaves that alone, as AUTOMATIC -> AUTOMATIC does above. A restored
    // depth is not a raise - no health count - but since 0.99.83 it is SAID, in the Sinks rail and
    // the log: the Milan report on 0.99.81 was a launch that silently restored 960 ms, which reads
    // as "the audio is a second behind the spectrum for no reason", and neither the user nor the
    // report could tell a restored deep lead from a fresh 120 ms start.
    {
        const int remembered = cascade::sink::validAudioBufferSetting(cfg.audioBufferAutoMs)
                                   ? cfg.audioBufferAutoMs
                                   : 0;
        if (audioBufferMs_ == 0) {
            const int depth = std::clamp(remembered,
                                         cascade::sink::audioLeadMs(cascade::sink::AudioOut::kPrimeFrames),
                                         960);
            if (remembered != 0 && depth > audioLeadNowMs()) {
                pipeline_.audio().setLeadFrames(cascade::sink::audioLeadFrames(depth));
                for (const auto& entry : patchDests_) {
                    if (entry.second && entry.second->leadMs() > 0) { entry.second->setLeadMs(depth); }
                }
                cascade::core::diagLogf(
                    "audio: buffer restored to %d ms (the depth automatic reached when it last ran)",
                    depth);
                audioLeadNote_ = AudioLeadNote::kRestored;
                audioLeadNoteMs_ = depth;
                audioLeadNoteCount_ = 0;
                audioLeadQuietMinutes_ = 0;   // the quiet run starts over at the restored lead
                patchQuietMinutes_.clear();
            }
            if (remembered > audioBufferAutoMs_) { audioBufferAutoMs_ = remembered; }
        } else {
            audioBufferAutoMs_ = remembered;
        }
    }
    squelchDb_ = cfg.squelchDb;
    pipeline_.setSquelchDb(squelchDb_);
    squelchAutoRestore(cfg.squelchAuto);   // AUTOMATIC squelch (0.99.74; stub campaign B12): quietly - a restored setting is not a decision to log
    // THE BIAS TEE IS SEEDED HERE, not inside the device branch below, so
    // that a config whose saved radio does not open (unplugged, or still on
    // its vendor driver) does not have the setting quietly rewritten to false
    // by the next save. adoptDeviceMirrors is what applies it to a radio that
    // does open - every one of these drivers switches the bias tee off during
    // open(), so it has to be re-applied afterwards or a mast-head amplifier
    // goes dark on every launch. It is PER RADIO (engine/bias_tee.hpp,
    // BiasTeePanel::remembered): each radio gets back only its own.
    biasTeePanel_.remembered = cfg.biasTee;
    // THE PLUTO'S ADDRESS IS SEEDED HERE TOO, and it has to be before the
    // scanNative() further down: that is what builds the Pluto's row, the
    // row's args are "uri=" plus this box, and the restore below finds the
    // row to point the combo at by comparing args. Seeded from the config
    // even when the saved source is something else entirely, because the box
    // is the user's own typing and belongs to them, not to the session that
    // happened to open a radio.
    cascade::core::formatUtf8(plutoUri_, sizeof(plutoUri_), "%s", cfg.plutoUri.c_str());
    // The rtl_tcp server's address, seeded for the same reason and before the
    // same scanNative(): its row's args are "rtltcp=" plus this box.
    cascade::core::formatUtf8(rtlTcpAddr_, sizeof(rtlTcpAddr_), "%s", cfg.rtlTcpAddr.c_str());

    // P7 settings. All are pure DSP switches with no failure mode, and the
    // loader has already clamped every one of them into range.
    deemphIndex_ = cfg.deemphasisIndex;
    pipeline_.setDeemphasisUs(kDeemphUs[deemphIndex_]);
    stereoEnabled_ = cfg.stereoEnabled;
    pipeline_.setStereoEnabled(stereoEnabled_);
    nrEnabled_ = cfg.nrEnabled;
    nrStrength_ = cfg.nrStrength;
    pipeline_.setNoiseReductionStrength(nrStrength_);
    pipeline_.setNoiseReductionEnabled(nrEnabled_);
    notchFreqHz_ = static_cast<float>(cfg.notchFreqHz);
    notchQ_ = static_cast<float>(cfg.notchQ);
    pipeline_.setNotchFrequencyHz(cfg.notchFreqHz);
    pipeline_.setNotchQ(cfg.notchQ);
    notchEnabled_ = cfg.notchEnabled;
    pipeline_.setNotchEnabled(notchEnabled_);
    autoNotch_ = cfg.autoNotch;
    pipeline_.setAutoNotchEnabled(autoNotch_);
    // applyConfig runs AFTER the startup loadBandPlan(), so a restored
    // selection that differs from the default has to re-load or the user's
    // chosen region silently reverts to "world" on every launch. Guarded on
    // change so the common case does not parse the directory twice.
    if (bandPlanSelection_ != cfg.bandPlanSelection) {
        bandPlanSelection_ = cfg.bandPlanSelection;
        loadBandPlan();
    }
    // THE TRANSMITTER'S SETTINGS, AND NOT ITS KEY. Everything restored here
    // is HOW it would transmit; nothing restored here can MAKE it transmit,
    // and there is nothing in AppConfig that could (core/config.hpp says why
    // at length). transmitOpen_ is cleared by startupState() before this runs
    // on a launch, so the page records what was showing at the last exit and
    // opens nothing by itself.
    transmitOpen_ = cfg.transmitOpen;
    transmitModeIndex_ = static_cast<int>(cascade::dsp::txModeFromIndex(cfg.transmitMode));
    transmitInputIndex_ = static_cast<int>(cascade::core::txInputFromIndex(cfg.transmitInput));
    transmitPowerDb_ = cfg.transmitPowerDb;
    transmitSplit_ = cfg.transmitSplit;
    transmitSplitHz_ = cfg.transmitSplitHz;
    transmitToneHz_ = cfg.transmitToneHz;
    transmitMonitor_ = cfg.transmitMonitor;
    transmitArgs_ = cfg.transmitArgs;
    transmitter_.setMode(cascade::dsp::txModeFromIndex(transmitModeIndex_));
    transmitter_.setInput(cascade::core::txInputFromIndex(transmitInputIndex_));
    transmitter_.setToneHz(transmitToneHz_);
    // The receiver's own position from the last session (the map pages are
    // told it by the window's half of the restore, AppWindow::applyConfig).
    rxSet_ = cfg.rxPositionSet;
    rxLat_ = cfg.rxLatDeg;
    rxLon_ = cfg.rxLonDeg;

    // The plugin store. Restoring the URL does NOT start a fetch - see
    // AppConfig::pluginCatalogueUrl. The catalogue is read only when the
    // user opens the store (once a session) or presses CHECK NOW. The
    // window's open flag arrives cleared (startupState): the store opens
    // from its rail key, never by itself - which is what keeps its
    // first-open read from ever being a startup fetch.
    pluginCatalogueUrl_ = cfg.pluginCatalogueUrl;
    // THE PLUGIN SITES (0.99.74): the addresses of other people's indexes the store lists beside the official catalogue. The kept copy of
    // each is merged under the official part of the list now (no network): the constructor loaded the kept official copy before it knew them.
    pluginSites_ = cfg.pluginSites;
    siteStatus_.clear();
    loadCachedSites();
    // Restored purely so it can be saved back unchanged when the user never
    // browses this session. Nothing reads it to decide whether to fetch.
    pluginLastUpdateCheck_ = cfg.pluginLastUpdateCheck;
    // Tune grants are pushed into PluginUi immediately, not just stored: the
    // plugin scan already ran in the constructor, so a tracker created during
    // it may call request_tune on its very first poll — before any rebuild
    // would have re-applied them.
    pluginTuneAllowed_ = cfg.pluginTuneAllowed;
    applyPluginTuneGrants();
    // Host API level 1: the settings grant, and the plugins' own settings -
    // loaded into the live store BEFORE the refreshPluginRunner below
    // re-attaches every plugin, so a plugin reading its settings from
    // attach() finds them there.
    pluginSettingsAllowed_ = cfg.pluginSettingsAllowed;
    applyPluginSettingsGrants();
    // Host API level 2: the TRANSMIT grant, applied on the same terms. The arming window is NOT in the config and therefore not here: a freshly loaded config always leaves it
    // CLOSED, whatever the engine was doing before (a configuration loaded mid-session closes it and unkeys too), so a restored grant is never a restored authorisation.
    pluginTransmitAllowed_ = cfg.pluginTransmitAllowed;
    applyPluginTransmitGrants();
    txPluginEnd("a configuration was loaded");
    pluginSettings_ = cfg.pluginSettings;
    pluginApi_->loadSettings(pluginSettings_);
    pluginSettingsGen_ = pluginApi_->settingsGeneration();
    // STOPS ARE APPLIED, not merely stored, for a stronger version of the same
    // reason: the scan in the constructor has already built every plugin's
    // instances against the default source, so a plugin the user stopped last
    // session is running right now. refreshPluginRunner tears that set down and
    // rebuilds it without the stopped ones, which is also what the restored
    // source needs; doing it here means a stopped plugin never survives a
    // launch even for a frame.
    //
    // HOW EACH PLUGIN RUNS (0.99.73), and the one-time migration of the old stop list (restorePluginRun).
    restorePluginRun(cfg);
    // AppConfig::closedWindows is no longer applied. Until 0.79.1 every plugin
    // window appeared by itself and this list kept the ones the user had shut
    // from coming back; now no plugin window appears until the user opens it
    // from its row, so there is nothing for the list to hold back. It is
    // still read and written so older builds and this one agree on the file.
    // BEFORE the rebuild, because refreshPluginRunner is what rebuilds the
    // mute snapshot the overrides are baked into. Restored after the stops for
    // the same reason they are restored at all: a decoder the user silenced
    // last session must not come back audible for the seconds it takes them to
    // find the checkbox again.
    pluginMuteOverride_ = cfg.pluginMuteOverride;
    // The user's own presets, before the same rebuild: they are baked into
    // the preset snapshot (rebuildMuteStates) the bars and the mute read.
    userPresets_ = cfg.userPresets;
    refreshPluginRunner();

    for (int i = 0; i < 8; ++i) {
        if (cfg.mode == kModeNames[i]) {
            commitModeIndex(i);
            break;  // an unknown mode name keeps the construction default
        }
    }

    // THE CONVERTERS, BEFORE ANY SOURCE IS RESTORED: cfg.centerHz is the AIR
    // frequency the last session was on, and the radio below is told it
    // through its own converter (radioHzForSource / applyConverterForSource).
    // A sound card's converter written under its full ALSA name (before the
    // identity key) is moved to the key it is looked up by now
    // (gui::soundCardConverterKey); nothing else is touched.
    converters_ = cascade::gui::migrateSoundCardConverterKeys(cascade::core::sanitiseConverters(cfg.converters));
    // The crystal correction (0.99.56), BEFORE the source restore below: the
    // restored radio's install applies its own value (applyPpmForSource).
    ppmCorrectionOn_ = cfg.ppmCorrection;
    ppmValues_ = cascade::core::sanitisePpmValues(cfg.ppm);
    // Each Airspy's gain mode, gains and decimation, put back by
    // adoptDeviceMirrors on that radio's own open, like the bias tee.
    airspyMemory_ = cfg.airspy;
    // Each radio's Auto gain switch, put back by adoptDeviceMirrors on that
    // radio's own open (0.99.73). Seeded here, before any open, so a saved
    // radio that does not open this launch keeps its entry for the next save.
    autoGainMemory_ = cfg.autoGainByRadio;
    // Each radio's manual gain (0.99.75), put back by adoptDeviceMirrors on that radio's
    // own open - seeded here for the same reason as the switch above.
    manualGainMemory_ = cfg.manualGainByRadio;

    // Source restore. The generator is always safe (it is already active);
    // a file is restored only if the path still opens; a Soapy device only
    // if its args re-open. Any failure falls back to the generator silently
    // except for lastError shown once in the Source section (sourceError_).
    //
    // The sound card's settings come back whatever the source is, so the row
    // shows what was set up last time (the same rule as the I/Q file's path).
    soundCard_ = cascade::gui::soundCardFromConfig(cfg.soundCard);
    if (cfg.sourceKind == "soundcard") {
        // ON A WORKER, like every other sound card open: the first frame is
        // drawn on the generator and the card replaces it when it answers
        // (app_window_soundcard.cpp). Until then - and for the rest of the
        // session if it never does - the config goes on naming it; the combo
        // names it too only once the open has actually failed.
        restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(
            cfg.sourceKind, cfg.soapyArgs, cfg.nativeArgs, cfg.iqFilePath, cfg.sampleRateHz);
        soundCardRemembered_ = soundCard_;  // the card the config names, whatever the section becomes
        restoreKeepLabel_.clear();
        sourceSel_ = kSoundCardRow;
        launchSoundCardOpen(/*restore=*/true, soundCard_);
        cascade::core::diagLogf("source: restoring the sound card (%s)",
                                cascade::source::loggableSoundCardDescription(soundCard_).c_str());
    } else if (cfg.sourceKind == "file") {
        auto file = std::make_unique<cascade::source::IqFileSource>();
        if (file->open(cfg.iqFilePath)) {
            const double fileRadioHz = radioHzForSource("file", std::string(), cfg.centerHz);
            if (fileRadioHz >= 0.0) { file->setCenterFrequencyHz(fileRadioHz); }
            iqOpenPath_ = cfg.iqFilePath;
            // No open can be in flight during the startup restore, but the
            // counter's contract is "every install bumps it" — an invariant
            // with an exception in it is one nobody can rely on later.
            ++sourceGen_;
            installSource(std::move(file));
            sourceKind_ = "file";
            applyConverterForSource();
            sourceSel_ = 1;
            followInputRate();
            cascade::core::diagLogf("source: restored an I/Q file at %.0f S/s",
                                    pipeline_.activeSource().sampleRateHz());
        } else {
            sourceError_ = file->lastError();
            // THE FACT, NEVER THE PATH - and the reason is five lines up in the
            // Open handler: a file name is the user's own data. The source's
            // error string is "cannot open file: <full absolute path>", and
            // this line goes to the ring, which means it goes into every crash
            // report, every hang report and the Copy diagnostics bundle. The
            // full text stays in sourceError_, which is shown on screen to the
            // person who already knows what they opened.
            cascade::core::diagWarnf("source: the saved I/Q file did not reopen");

            // ...AND THE CONFIG GOES ON NAMING IT, exactly as it does for a
            // radio that did not reopen (see the device branch below). A
            // recording goes missing the same ways a dongle does - an
            // external drive unplugged, a folder renamed, the capture moved -
            // and until 0.94.1 this branch left iqOpenPath_ empty, so the
            // exit save wrote sourceKind "siggen" with iqFilePath BLANK: one
            // session with the drive out and FoxSDR could not even say which
            // file it had been playing. The saved path is remembered instead,
            // and the box is filled in with it so the user can plug the drive
            // back in and press Open without typing it again.
            restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(
                cfg.sourceKind, cfg.soapyArgs, cfg.nativeArgs, cfg.iqFilePath, cfg.sampleRateHz);
            // WHAT THE SOURCE SECTION CALLS IT: the file's own name, not the
            // whole path, because the preview it goes into is one combo wide
            // and a recording lives several folders deep. Never logged - the
            // rule above is about the ring, and this string only ever reaches
            // the screen of the person who chose the file.
            restoreKeepLabel_ = cascade::gui::fileNameOf(cfg.iqFilePath);
            // Nothing is ticked in the dropdown, for the same reason the
            // device branch gives: the generator is not what the user chose.
            sourceSel_ = -1;
        }
    } else if ((cfg.sourceKind == "soapy" && !cfg.soapyArgs.empty()) ||
               (isNativeSourceKind(cfg.sourceKind) && !cfg.nativeArgs.empty())) {
        // The native list has to exist before the rule below can read it, and
        // nothing has drawn a frame yet. It costs a SetupAPI walk and opens
        // nothing (scanNative), so it is safe here where the Soapy scan
        // deliberately is not.
        scanNative();
        std::string kind = cfg.sourceKind;
        std::string args = (kind == "soapy") ? cfg.soapyArgs : cfg.nativeArgs;
        std::string fallbackSoapyArgs;

        // PREFER THE NATIVE DRIVER, AUTOMATICALLY, AND SAY SO. A config
        // written before 0.91.0 says "soapy, driver=rtlsdr" because that was
        // the only way to reach the dongle; the user is not going to reopen
        // the Source section to switch over, and should not have to. See
        // gui::preferNativeFor for what "the same dongle" means and why a
        // saved serial has to match.
        if (const std::optional<cascade::source::NativeDeviceInfo> nat =
                cascade::gui::preferNativeFor(cfg.sourceKind, cfg.soapyArgs, nativeDevices_)) {
            cascade::core::diagLogf(
                "source: opening %s natively (was SoapySDR %s)",
                modelFromNativeLabel(nat->label).c_str(),
                cascade::core::sanitiseDevice(cfg.soapyArgs).c_str());
            fallbackSoapyArgs = cfg.soapyArgs;
            kind = nat->driver;
            args = nat->args;
        }

        // Seeded BEFORE the open, because openDeviceSync applies it as part of
        // bringing the device up - the port has to be right from the first
        // sample, not corrected afterwards.
        deviceAntenna_ = cfg.soapyAntenna;
        auto dev = openDeviceSync(kind, args, cfg.sampleRateHz);
        if (!dev && !fallbackSoapyArgs.empty() &&
            cascade::gui::nativeOpenShouldFallBack(sourceError_)) {
            // The dongle is one whose tuner the native driver does not
            // support (E4000, FC0012/13). It opened perfectly well through
            // SoapySDR before this release and must go on doing so.
            cascade::core::diagWarnf(
                "source: the native %s driver refused the saved radio (%s); opening it "
                "through SoapySDR instead",
                kind.c_str(), sourceError_.c_str());
            const std::string nativeKey = cascade::core::converterRadioKey(kind, args);
            kind = "soapy";
            args = fallbackSoapyArgs;
            dev = openDeviceSync(kind, args, cfg.sampleRateHz);
            // The radio the user chose, reached another way: its converter
            // comes with it (before the saved frequency is converted below).
            if (dev) { noteConverterFallback(nativeKey, cascade::core::converterRadioKey(kind, args)); }
        }
        if (dev) {
            // The saved AIR frequency, told to the radio through the
            // converter remembered for it (the radio has not been installed
            // yet, so the pipeline's view cannot do it here).
            // A frequency the converter cannot deliver (0 Hz or below at the
            // radio) is not sent at all; the radio stays at its own default.
            const double radioHz = radioHzForSource(kind, args, cfg.centerHz);
            if (radioHz > 0.0 || !cascade::core::converterActive(converterForKey(
                                     cascade::core::converterRadioKey(kind, args)))) {
                // Through the radio's crystal correction too (0.99.56).
                dev->setCenterFrequencyHz(
                    ppmPreTellHz(kind, args, dev->hasFrequencyCorrection(), radioHz));
            }
            device_ = dev.get();
            soapyView_ = dynamic_cast<cascade::source::SoapySource*>(device_);
            deviceArgs_ = args;
            deviceModel_ = (kind == "soapy") ? cascade::core::sanitiseDevice(args)
                                             : modelFromNativeLabel(nativeLabelFor(kind, args));
            if (kind == "soapy") {
                cfgSoapyArgs_ = args;
                cfgNativeArgs_ = cfg.nativeArgs;
            } else {
                cfgNativeArgs_ = args;
                // THE SOAPY ARGS ARE KEPT even though a native driver is what
                // opened: they are what the prefer-native rule reads on the
                // NEXT launch, and throwing them away would make the first
                // native session the last one that could ever fall back.
                cfgSoapyArgs_ = cfg.soapyArgs;
            }
            ++sourceGen_;  // same invariant as the file branch above
            installSource(std::move(dev));
            sourceKind_ = kind;
            // THE SAVED RADIO OPENED (0.99.64, core/health_events.hpp): counted
            // like any other open. This runs BEFORE the application has read
            // whether usage reporting is on, and the ledger holds what it is
            // given until it has (telemetryStartup).
            cascade::core::health::noteRadioOpen(kind);
            healthWatchSamples(kind);
            applyConverterForSource();
            // Point the combo at the restored device if this machine still
            // enumerates it; -1 otherwise (preview falls back to live name).
            sourceSel_ = -1;
            if (kind == "soapy") {
                for (std::size_t i = 0; i < soapyDevices_.size(); ++i) {
                    if (soapyDevices_[i].args == args) {
                        sourceSel_ = soapyRowBase() + static_cast<int>(i);
                    }
                }
            } else {
                for (std::size_t i = 0; i < nativeDevices_.size(); ++i) {
                    if (nativeDevices_[i].args == args) {
                        sourceSel_ = kNativeRowBase + static_cast<int>(i);
                    }
                }
            }
            followInputRate();
            cascade::core::diagLogf("source: restored %s (%s) at %.0f S/s",
                                    deviceModel_.c_str(), kind.c_str(),
                                    pipeline_.activeSource().sampleRateHz());
        } else {
            // THE SAVED RADIO WOULD NOT OPEN, counted anonymously (0.99.64,
            // core/health_events.hpp): the driver kind and a reason class,
            // nothing of the message - which quotes the device arguments back.
            // The most common failure there is, and the first thing this run does.
            cascade::core::health::noteRadioFail(kind, sourceError_);
            // WHY, AS ONE WORD (0.99.92): the same reason class the count above files it under, and the only part of the driver's message that
            // may go in the log - the message itself quotes the device arguments back (below). Taken HERE, before the sentence at the end of this
            // branch wraps sourceError_ in the radio's name. The 0.99.91 field report had to be read off the anonymous counts to learn that the
            // saved radio was merely busy; the log said only that it "did not reopen".
            const std::string whyClass = cascade::core::health::radioFailWhy(kind, sourceError_);
            const bool failedBusy = cascade::core::health::classifyRadioOpen(kind, sourceError_) == cascade::core::health::RadioReason::Busy;
            // openDeviceSync already set sourceError_. A radio that was there last
            // session and is not there now is the single most common support
            // question this product gets - but the driver's own message quotes
            // the device ARGUMENTS back, and those carry the serial number.
            // Same rule as the line above and as every other place these
            // strings are recorded: the sanitised model, never the raw args.
            //
            // THE MODEL OF THE RADIO THAT WAS TRIED (0.99.36). This used to
            // print cfg.soapyArgs, which is empty for every native radio, so
            // the field logs read "the saved radio (, sdrplay) did not
            // reopen" and named nothing.
            std::string triedModel = (kind == "soapy")
                                         ? cascade::core::sanitiseDevice(args)
                                         : modelFromNativeLabel(nativeLabelFor(kind, args));
            if (triedModel.empty() || triedModel == args) { triedModel = kind; }
            cascade::core::diagWarnf(
                "source: the saved radio (%s, %s) did not reopen (%s) - the receiver is on the "
                "signal generator and the radio stays saved",
                triedModel.c_str(), kind.c_str(), whyClass.c_str());

            // ...AND THE CONFIG GOES ON NAMING IT. The session runs on the
            // generator above, which is right - it has to run on something -
            // but the exit save reads sourceKind_ and the cfg*Args_ mirrors,
            // and those describe the generator. Before this, ONE session with
            // the dongle unplugged, held by another program, or held by a
            // second copy of FoxSDR (the report that found this) wrote back
            // "siggen" with both args slots empty and the radio was gone for
            // good. The SAVED values are remembered here, unchanged, so the
            // next start tries exactly what this one tried. See
            // gui::rememberedSourceAfterFailedOpen and gui::sourceToSave.
            restoreKeep_ = cascade::gui::rememberedSourceAfterFailedOpen(
                cfg.sourceKind, cfg.soapyArgs, cfg.nativeArgs, cfg.iqFilePath, cfg.sampleRateHz);

            // WHAT THE SOURCE SECTION CALLS IT. The enumerated label is the
            // best name when this machine can still see the radio (in use by
            // another program is exactly that case); when it cannot,
            // nativeLabelFor hands back the args verbatim, which for a native
            // device is nothing but a serial - so the driver key is used
            // instead. Not a privacy rule like the log line above, because
            // this string never leaves the screen; it is simply not a name.
            std::string label = (kind == "soapy") ? cascade::core::sanitiseDevice(args)
                                                  : modelFromNativeLabel(nativeLabelFor(kind, args));
            if (label.empty() || label == args) { label = kind; }
            restoreKeepLabel_ = label;
            // A RADIO THAT WAS ONLY BUSY IS TRIED AGAIN once the engine is running (0.99.92, engine/saved_radio_retry.hpp): the previous FoxSDR,
            // still letting go of the dongle after the in-app updater, is the case that was found. Armed for BUSY alone; the stamp is the retry
            // clock's now, so the first try is 2 s after this failure.
            if (failedBusy && restoreKeep_.valid()) {
                savedRadioRetryBusy_ = true;
                savedRadioRetryAttempts_ = 0;
                savedRadioRetryLastFailSec_ = retryClockS();
                cascade::core::diagLogf(
                    "source: the saved radio is in use by another program - trying it again in "
                    "2, 4, 8 and 16 s");
            }
            // Which radio, why, and where the receiver is - the driver's own
            // reason is kept verbatim inside the sentence (0.99.36).
            sourceError_ = cascade::gui::radioNotOpenedSentence(label, sourceError_);
            // NOTHING IS TICKED IN THE DROPDOWN. -1 is the same "the live
            // source is not one of these rows" the Refresh path uses; the
            // preview names the saved radio instead, so the generator is
            // never shown as though the user had picked it.
            sourceSel_ = -1;
        }
    }
    if (sourceKind_ == "siggen") {
        // Generator kept (or fallen back to): carry the saved center so the
        // readout matches the last session. Nominal-center set cannot fail -
        // unless the user set a converter on the generator that cannot
        // deliver that air frequency, which then leaves the nominal alone.
        applyConverterForSource();
        pipeline_.activeSource().setCenterFrequencyHz(cfg.centerHz);
    }


    // VFO after the source/rate restore so the clamps use the REAL rates the
    // chain ended up with, not whatever the file claimed.
    const double bwHi = kVfoBwMaxChanFrac * pipeline_.channelRateHz();
    vfoBandwidthHz_ = std::max(kVfoBwMinHz, std::min(cfg.bandwidthHz, bwHi));
    pipeline_.setVfoBandwidthHz(vfoBandwidthHz_);
    // The combo's tick, and -1 for a saved bandwidth that is none of the steps
    // - a config written while a plugin preset had the VFO at its own width.
    // The combo letters vfoBandwidthHz_ itself either way.
    bandwidthIndex_ = bandwidthStepIndex(vfoBandwidthHz_);
    double off = cfg.vfoOffsetHz;
    const double lim = 0.5 * pipeline_.inputRateHz() - 0.5 * vfoBandwidthHz_;
    off = (lim > 0.0) ? std::clamp(off, -lim, lim) : 0.0;
    pipeline_.setVfoOffsetHz(off);
    vfoOffsetKhz_ = static_cast<float>(off / 1000.0);
    // The context a report describes the session by is rendered NOW, with the config applied (the window used to render it
    // at the top of run(); from here the engine is the one writer).
    diagContextHave_ = false;
    pumpDiagContext();
}

// THE RECEIVER'S HALF OF THE CONFIG SNAPSHOT (engine stage 3a): every line of
// AppWindow::currentConfig that read the receiver, in their order. Every field
// of AppConfig is written by exactly one half. Not const: the usage report's
// journal (telemetryJournal) accrues the session as it is written.
void Engine::fillConfig(cascade::core::AppConfig& cfg) {
    // The web server's and the CAT listener's fields (stage 5a; the only writer since 5b - the window's own copies are gone).
    servers_.store(cfg);
    // WHICH SOURCE THE FILE NAMES, which is not always the one that is
    // running: a restore that could not open the saved radio leaves the
    // generator installed and the radio remembered, and the radio is what
    // goes back into the file. Everything else - a clean restore, any
    // deliberate switch - is the live source, exactly as before.
    // WHILE THE PATCH PAGE HOLDS THE RECEIVER'S RADIO (0.99.17) the receiver
    // runs on the generator only because the page borrowed its radio, so the
    // radio is what is saved - the same rule as a restore that could not
    // open it, applied here rather than by borrowing that state, which would
    // also relabel the Source panel.
    cascade::gui::RememberedSource keep = restoreKeep_;
    if (patchMainKeep_.valid) {
        keep = cascade::gui::RememberedSource{};
        keep.kind = patchMainKeep_.kind;
        if (patchMainKeep_.kind == "soapy") {
            keep.soapyArgs = patchMainKeep_.args;
            keep.nativeArgs = cfgNativeArgs_;
        } else if (patchMainKeep_.kind == "soundcard") {
            // A lent sound card is named by cfg.soundCard (below, the card as
            // it was lent - patchMainKeep_.card); both radio slots keep what
            // they had.
            keep.soapyArgs = cfgSoapyArgs_;
            keep.nativeArgs = cfgNativeArgs_;
        } else {
            keep.nativeArgs = patchMainKeep_.args;
            keep.soapyArgs = cfgSoapyArgs_;
        }
        keep.sampleRateHz = patchMainKeep_.rateHz;
    }
    const cascade::gui::SavedSource src = cascade::gui::sourceToSave(
        sourceKind_, cfgSoapyArgs_, cfgNativeArgs_, iqOpenPath_,
        pipeline_.activeSource().sampleRateHz(), keep);
    cfg.sourceKind = src.kind;
    cfg.soapyAntenna = deviceAntenna_;
    // ONE SLOT PER FAMILY, and BOTH are written on every save - not just the
    // one belonging to whatever is open. The Soapy args of a radio now being
    // driven natively are what the prefer-native rule reads on the next
    // launch and what the tuner fallback needs; dropping them the moment the
    // native driver takes over would make the first native session the last
    // one that could ever fall back. See AppConfig::nativeArgs.
    cfg.soapyArgs = src.soapyArgs;
    cfg.nativeArgs = src.nativeArgs;
    cfg.lookForNetworkUsrps = lookForNetworkUsrps_;
    cfg.biasTee = biasTeePanel_.remembered;
    // Every radio's converter, including those not open now: a converter is
    // part of how that radio is cabled, and must survive a session without it.
    cfg.converters = converters_;
    // Every Airspy's gain mode, gains and decimation, open now or not.
    cfg.airspy = airspyMemory_;
    // Each radio's Auto gain switch (0.99.73) and manual gain (0.99.75): kept in the radio's own map, open now or not.
    // The audio buffer: the chosen value (0 = AUTOMATIC) and the depth automatic reached (0.99.73).
    cfg.audioBufferMs = audioBufferMs_;
    cfg.audioBufferAutoMs = audioBufferAutoMs_;
    // The crystal correction (0.99.56): the switch and every radio's value.
    cfg.ppmCorrection = ppmCorrectionOn_;
    cfg.ppm = ppmValues_;
    cfg.autoGainByRadio = autoGainMemory_;
    cfg.manualGainByRadio = manualGainMemory_;
    // WHAT IS IN THE BOX, not what opened. A Pluto that is on the bench has
    // its address in nativeArgs as well; this field is the typing, and it has
    // to survive a launch in which the board never answered so it can be
    // corrected next time rather than retyped from nothing.
    cfg.plutoUri = plutoUri_;
    cfg.rtlTcpAddr = rtlTcpAddr_;
    // THE FILE THIS SESSION PLAYED, or the one it could not find. Normally
    // iqOpenPath_, which is the last recording that actually opened; when the
    // saved source was a file and the restore could not open it, the same
    // decision that keeps a missing radio in the config keeps the path (see
    // gui::sourceToSave). Before that, one session with the drive unplugged
    // wrote this out EMPTY - the path box came back blank on the next start
    // and nothing anywhere said which file had gone.
    cfg.iqFilePath = src.filePath;
    // THE SOUND CARD THAT RAN, never the Source section's unopened edits: the
    // next launch opens exactly what is written here. The running card; the
    // card the patch page has borrowed; the card a failed restore, a re-Open's
    // release or a hand-back left remembered; and only when there is none of
    // those, the section's settings - the card as last set up
    // (gui::soundCardToSave).
    cfg.soundCard = cascade::gui::soundCardToConfig(cascade::gui::soundCardToSave(
        sourceKind_, soundCardLive_, patchMainKeep_.valid && patchMainKeep_.kind == "soundcard",
        patchMainKeep_.card, restoreKeep_.kind == "soundcard", soundCardRemembered_, soundCard_));
    cfg.centerHz = pipeline_.activeSource().centerFrequencyHz();
    cfg.mode = kModeNames[modeIndex_];
    cfg.bandwidthHz = vfoBandwidthHz_;
    cfg.squelchDb = squelchDb_;
    cfg.squelchAuto = squelchAuto_;
    cfg.volume = volume_;
    cfg.dbMin = dbMin_;
    cfg.dbMax = dbMax_;
    cfg.vfoOffsetHz = pipeline_.vfoOffsetHz();
    // The live rate, EXCEPT when the source being saved is a remembered radio
    // rather than the generator standing in for it: the generator's fixed
    // 2 MS/s is not a rate the user ever chose for their receiver, and it is
    // the number the next start would hand to the driver's open().
    cfg.sampleRateHz = src.sampleRateHz;
    cfg.stereoEnabled = stereoEnabled_;
    cfg.deemphasisIndex = deemphIndex_;
    cfg.nrEnabled = nrEnabled_;
    cfg.nrStrength = nrStrength_;
    cfg.notchEnabled = notchEnabled_;
    cfg.notchFreqHz = static_cast<double>(notchFreqHz_);
    cfg.notchQ = static_cast<double>(notchQ_);
    cfg.autoNotch = autoNotch_;
    cfg.bandPlanSelection = bandPlanSelection_;
    cfg.transmitOpen = transmitOpen_;
    cfg.transmitMode = transmitModeIndex_;
    cfg.transmitInput = transmitInputIndex_;
    cfg.transmitPowerDb = transmitPowerDb_;
    cfg.transmitSplit = transmitSplit_;
    cfg.transmitSplitHz = transmitSplitHz_;
    cfg.transmitToneHz = transmitToneHz_;
    cfg.transmitMonitor = transmitMonitor_;
    cfg.transmitArgs = transmitArgs_;
    // AND NOTHING FOR THE KEY: the key rules' state (who holds it, the
    // latch, the marks, the consent) and the transmitter's latch are not written, because AppConfig has nowhere
    // to put them and must not grow one. A saved key is a radio that comes up transmitting.
    cfg.rxPositionSet = rxSet_;
    cfg.rxLatDeg = rxLat_;
    cfg.rxLonDeg = rxLon_;
    cfg.pluginCatalogueUrl = pluginCatalogueUrl_;
    cfg.pluginSites = pluginSites_;
    cfg.pluginLastUpdateCheck = pluginLastUpdateCheck_;
    cfg.pluginTuneAllowed = pluginTuneAllowed_;
    cfg.pluginSettingsAllowed = pluginSettingsAllowed_;
    cfg.pluginTransmitAllowed = pluginTransmitAllowed_;   // the grant is saved; the arming window never is (stub campaign B13)
    cfg.pluginSettings = pluginSettings_;
    // The old file-name list is dropped (0.99.73): applyConfig migrated it, by id, into pluginRun, and an older build that reads this file
    // sees nothing stopped rather than a stale list. AUTO is the default and is never written.
    cfg.pluginsStopped.clear();
    cfg.pluginRun.clear();
    for (const auto& [id, mode] : pluginRun_) {
        if (mode != cascade::core::PluginRun::Auto) { cfg.pluginRun[id] = cascade::core::pluginRunWord(mode); }
    }
    // closedWindows is written empty: nothing reads it since 0.79.1 (see
    // applyConfig), and an empty list is what an older build would take to
    // mean "no window was shut" - the nearest true statement it can make
    // about a session in which windows only ever open by hand.
    cfg.closedWindows.clear();
    cfg.pluginMuteOverride = pluginMuteOverride_;
    cfg.userPresets = userPresets_;
    telemetryJournal(cfg);
    // FALSE while running, so a start-up that reads it back knows the previous
    // session never got as far as writing true. Set only on the clean exit
    // path, which is what makes an absent marker mean "crashed".
    cfg.telemetryCleanExit = telemetryCleanExit_;
}

// THE ENGINE'S START-UP (engine stage 3a): what gui::AppWindow's constructor
// did to the receiver before its config restore, in the order it did it. The
// watchdog pause around a device open is the host's (pause/resumeWatchdog).
// (The spectrum's range is the window's own: it follows the state's.) Called once, before
// applyConfig and before the first pump.
void Engine::initialise() {
    recordDir_ = defaultRecordDir();
    // Demo signal until real sources land (P4): two tones at distinct offsets
    // and levels over a noise floor, so both display axes are visibly
    // exercised — frequency (two peaks left and right of center) and
    // amplitude (different heights / waterfall colors).
    cascade::source::SigGen& gen = pipeline_.sigGen();
    gen.setTone(0, 300000.0, -30.0f);
    gen.setTone(1, -500000.0, -45.0f);
    gen.setNoiseFloorDb(-90.0f);

    // Park the VFO on demo tone 0 so the receiver is tuned to something from
    // the first Play: WFM (the default mode) renders an unmodulated carrier
    // as near-silence, and switching to CW yields the 700 Hz sidetone.
    pipeline_.setVfoOffsetHz(1000.0 * static_cast<double>(vfoOffsetKhz_));
    pipeline_.audio().setVolume(volume_);
    // Push every P7 mirror once so the pipeline and the panels start in
    // agreement even when no config file exists (the pipeline's own defaults
    // match these, so this is belt and braces rather than a fix-up).
    pipeline_.setStereoEnabled(stereoEnabled_);
    pipeline_.setNoiseReductionEnabled(nrEnabled_);
    pipeline_.setNoiseReductionStrength(nrStrength_);
    pipeline_.setNotchEnabled(notchEnabled_);
    pipeline_.setNotchFrequencyHz(static_cast<double>(notchFreqHz_));
    pipeline_.setNotchQ(static_cast<double>(notchQ_));
    pipeline_.setAutoNotchEnabled(autoNotch_);
    // Optional program data / optional user code. Both are silent no-ops when
    // their directory is absent, which is the normal case when running out of
    // a build tree — and every bounded --frames CI run takes this path.
    loadBandPlan();
    rescanPlugins();
    // THE FIRST SCAN IS DONE: the copies a session before this one could not delete (a module was mapped) are deleted now if this scan still
    // finds them superseded, and the store opens with the catalogue the last good read kept - no request is made (stub campaign B3; both run
    // before the first pass, like the scan beside them).
    processPendingPluginRemovals();
    loadCachedCatalogue();
    // The catalogue URL starts at the published default and is overwritten by
    // a config restore if the user (or an enterprise deployment) changed it.
    // Setting it here is NOT a fetch: nothing contacts the origin until CHECK
    // NOW is pressed in the plugin store window.
    pluginCatalogueUrl_ = cascade::core::AppConfig{}.pluginCatalogueUrl;
    // DELIBERATELY no SoapySDR enumeration here. Enumeration loads vendor
    // modules (SoapyUHD -> uhd.dll -> libusb) whose USB discovery faulted
    // in-process in ~2% of measured `--frames 1` runs (0xC0000005 inside
    // libusb-1.0.dll during uhd::device::find — P6a, 2026-08-15). The scan
    // now runs only on the user's explicit request (first Source-dropdown
    // open, or Refresh — scanSoapy()), so sessions that never touch Soapy —
    // including every bounded --frames CI run — never execute that code.

    // THE DEVICE OPEN IS BLOCKING WORK AND DOES NOT BELONG ON THIS THREAD.
    // engine/audio_open.hpp carries the field report and the whole argument; the
    // opener is Pipeline's, packaged so it can outlive this window, and the
    // hooks are the watchdog's for the bounded wait the requesting frame
    // spends. Bound here, before anything can ask for a device.
    audioOpen_.bind(pipeline_.audioOpener(), [this] { pauseWaitWatchdog(); },
                    [this] { resumeWaitWatchdog(); });
    // And the microphone, for the same reason: waveInOpen has no timeout
    // either. The opener is the Transmitter's and owns the microphone.
    micOpen_.bind(transmitter_.microphoneOpener(), [this] { pauseWaitWatchdog(); },
                  [this] { resumeWaitWatchdog(); });

    devices_ = pipeline_.audio().listOutputDevices();
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        if (devices_[static_cast<std::size_t>(i)].isDefault) { deviceIndex_ = i; }
    }
    if (deviceIndex_ < 0 && !devices_.empty()) { deviceIndex_ = 0; }
}

// The bookmark file (FreqManager's default path), read once at start-up by a
// front end whose run is persistent (gui::AppWindow gates it on its config
// path, as it always has - its constructor says why).
void Engine::loadBookmarks() {
    bookmarkPath_ = cascade::core::FreqManager::defaultPath();
    std::string bmErr;
    if (!freqMgr_.load(bookmarkPath_, bmErr)) {
        bookmarkError_ = bmErr;
        // KEPT ASIDE BEFORE ANYTHING CAN SAVE OVER IT (0.99.65, core/damaged_file.hpp); the red line stays up. If the rename is refused the
        // file stays exactly as it is and this session's saver writes nothing.
        const cascade::core::SetAsideResult r = cascade::core::setDamagedFileAside(bookmarkPath_);
        const unsigned long long bytes = static_cast<unsigned long long>(r.bytes);
        switch (r.outcome) {
            case cascade::core::SetAsideResult::Outcome::NothingToKeep:
                break;  // a first run, an empty file, or a folder: nothing to lose, nothing said
            case cascade::core::SetAsideResult::Outcome::KeptAside:
                // WHICH FILE AND HOW BIG, and no path: a log line never names one.
                cascade::core::diagLogf(
                    "bookmarks: a damaged file (%llu bytes) was kept aside beside the live one; the next save writes a new file", bytes);
                break;
            case cascade::core::SetAsideResult::Outcome::CouldNotKeep:
                bookmarkSaver_.forbidWrites("bookmarks: not saved - the damaged file could not be kept aside");
                cascade::core::diagWarnf(
                    "bookmarks: a damaged file (%llu bytes) could not be kept aside, so nothing will be saved over it in this session", bytes);
                break;
        }
    }
    // The waterfall's markers, under the same gate and beside them. A damaged file is reported in the Markers window, never on stdout, and
    // is kept aside the same way (0.99.65).
    markerPath_ = cascade::core::FreqMarkers::defaultPath();
    std::string mkErr;
    if (!freqMarkers_.load(markerPath_, mkErr)) {
        markerError_ = mkErr;
        const cascade::core::SetAsideResult r = cascade::core::setDamagedFileAside(markerPath_);
        const unsigned long long bytes = static_cast<unsigned long long>(r.bytes);
        switch (r.outcome) {
            case cascade::core::SetAsideResult::Outcome::NothingToKeep:
                break;
            case cascade::core::SetAsideResult::Outcome::KeptAside:
                cascade::core::diagLogf(
                    "markers: a damaged file (%llu bytes) was kept aside beside the live one; the next save writes a new file", bytes);
                break;
            case cascade::core::SetAsideResult::Outcome::CouldNotKeep:
                markerSaver_.forbidWrites("markers: not saved - the damaged file could not be kept aside");
                cascade::core::diagWarnf(
                    "markers: a damaged file (%llu bytes) could not be kept aside, so nothing will be saved over it in this session", bytes);
                break;
        }
    }
    markerSavedVersion_ = freqMarkers_.version();
}

// THE ENGINE'S TRANSFERS, CANCELLED (engine stage 3a): the first step of a
// teardown, taken before the front end's own transfers are cancelled
// (gui::AppWindow::~AppWindow), exactly where its destructor always took it.
void Engine::stopTransfers() {
    // A catalogue fetch or a plugin download may still be in flight. The
    // std::async futures below block in their own destructors until the
    // worker returns, so without this an app closed mid-download would sit
    // there, apparently hung, for as long as the transfer took. cancel() is
    // thread-safe by contract and makes the worker fail out with "cancelled",
    // deleting its temp file on the way — so teardown stays bounded and no
    // partial DLL is left behind. Harmless when nothing is running.
    pluginRepo_.cancel();
}

// THE ENGINE'S TEARDOWN (engine stage 3a): every worker reaped and the
// plugin system brought down in its stated order, then the recorders - the
// rest of what gui::AppWindow's destructor always did, in its order. Once:
// ~Engine runs stopTransfers and this itself for a front end that did not.
void Engine::teardown() {
    tornDown_ = true;
    // The servers first (stage 5b; shutdownQuiesce did it already on the product's path, and this is the same call on the paths that do not
    // go through it): no handler of either may be running while the engine comes apart.
    servers_.shutdown();
    // The control thread (stage 3b): nothing below may run beside it.
    stopControlThread();
    // Same problem, no cancel to reach for: a device open may still be inside
    // SoapySDR::Device::make(). See reapPendingDeviceOpen for the semantics.
    reapPendingDeviceOpen();
    // The sound card's list and open, on the same terms.
    reapSoundCardWorkers();

    // And the same problem again on the AUDIO device, which is the one that
    // produced it: an open still inside waveOutOpen would hold ~AudioOpen's
    // future - and therefore this destructor - for the rest of the driver
    // call. reap() spends a short grace and then abandons the worker; it is
    // safe to abandon because the opener holds the sink alive by shared_ptr
    // (see Pipeline::audioOpener).
    audioOpen_.reap();
    // The microphone's gate, same argument: waveInOpen has no timeout either,
    // and Transmitter::microphoneOpener owns the microphone it opens.
    micOpen_.reap();

    // The GPS reader's thread, if a read is still running when the window is
    // destroyed without run()'s teardown (a failed backend init, a test that
    // never entered the frame loop). Its own destructor would join too; this
    // makes the order explicit and bounded: one port read (200 ms) while it
    // is reading, GpsReader::kOpenAbandonWait (1 s) while it is still inside
    // the port driver's open - after which the worker is abandoned to finish
    // on its own, not joined, so a Bluetooth port whose puck is off cannot
    // hold this destructor for the length of an RFCOMM connect attempt.
    gpsReader_.stop();

    // And the same again for the lazy device SCAN, which blocks in
    // SoapySDR::Device::enumerate() and is the likelier of the two to be in
    // flight at quit — it starts the moment the source combo is opened.
    reapPendingSoapyScan();

    // THE PLUGIN SYSTEM COMES DOWN IN ONE STATED ORDER, HERE, and not as a
    // by-product of reverse member-declaration order.
    //
    // detachAndUnloadPlugins() exists because that order is the feature:
    // detach the runner from the pipeline, destroy the decoder instances,
    // then take the GUI half and the host services away, then unmap the
    // modules. Every OTHER path that unloads plugins (a rescan, removing an
    // installed plugin) calls it. This one did not - it let the members fall
    // apart on their own, and their declaration order had pluginUi_ destroyed
    // BEFORE pluginRunner_, which is the exact reverse. A decoder's destroy()
    // that asks the host anything therefore reached a destroyed PluginUi and
    // a freed host-API table: Survey Engine 0.1.0 does exactly that (it
    // timestamps the dwell it is finishing) and it crashed on 0.96.3 with an
    // access violation on Windows and an abort inside libc++ on Android.
    //
    // Idempotent: whatever it clears, the member destructors below find empty.
    // The patch's radios first, as in run()'s teardown - idempotent too.
    patchStopAll(false);
    detachAndUnloadPlugins();

    // The seam's thread, if the test asked for one (see holdAudioLockMs_): joined before anything it touches goes away.
    if (holdAudioLockThread_.joinable()) { holdAudioLockThread_.join(); }

    // Safety net (run()'s teardown already does this on the normal path):
    // the recorder members are destroyed before pipeline_ (reverse
    // declaration order), so any tap still installed must be uninstalled
    // first — stop*Recording clears the pipeline pointer, then finalizes.
    stopIqRecording();
    stopAudioRecording();
}

// --- THE PER-FRAME PUMP (engine stage 3a) ------------------------------------
//
// The engine's steps of the frame loop, in the order drawUi has always run
// them. gui::AppWindow::drawUi calls each at the point it always ran (its
// comments there say why each is where it is), with its own steps between
// them; pump() runs them all, in the same order, for a front end with no
// frame of its own (tests/test_engine_headless). GUI thread in stage 3a; the
// control thread's loop in 3b (docs/engine-stage3.md).

void Engine::pumpFrameBegin() {
    (void)receiverSnapshot_->retryInstall();
    drainLocalCommands();
    // The engine API's queue, after the local one (engine/fox_engine_api.hpp).
    api_->beginPass();
    // ...and what the front end posted to be done by the pass (postToPass): a bounded run's seams, the measurement harness.
    runPassWork();
}

void Engine::pumpInputs() {
    endTakesOnFault();
    pumpDecoderOutput();
}

// THE REMOTE CONTROLS' PHASE, in the place applyWebControls had in the frame (pump move step P1, F3): after the local queue and the
// API's (pumpFrameBegin) and the inputs (pumpInputs), before the plugins' requests and the scanner (pumpPlugins), so the order the
// operator could observe is the one the window's frame had - a local press landed in the same pass is applied first and a remote one
// meets its key (rule 6).
//
// STAGE 5d: THE PULL IS GONE, THE PHASE IS NOT. The web server and every CAT connection hold sessions on the table (net/web_session.hpp,
// net/cat_server.hpp), and what they submit goes to the INTERNAL TIER, drained here by ApiHost::applyTail - the web session's commands
// first, then CAT's, each session's in the order it submitted them - through the table's one `apply`, so the key rules (ApiHostImpl::rules_,
// single-threaded by contract) and the whole of applyCommand are still touched by the pumping thread alone. Why a tier of its own and not
// the queue beginPass drains: that queue is in ARRIVAL order, and a browser that arrived first in wall time would then win the key against
// a local press that landed in the same pass; the tier is drained here, so "after the operator at the machine, whenever it arrived" is
// structural (tests/test_remote_control_pull.cpp A4 holds it: the browser FIRST in wall time, the local press still wins). The server that
// stops, the page that is closed, the consent withdrawn - each is a session closed or a heartbeat that stopped, judged by the key rules
// (docs/engine-stage4.md 6.12), so there is no "is the server running" question left for this phase to ask.
void Engine::pumpRemoteControls() { api_->applyTail(); }

void Engine::pumpPlugins() {
    // An unload the interface could not take (it was not framing) is retried once it has framed (pump move step P3).
    retryPluginUnload();
    applyPluginApi();
    // WHICH PLUGINS RUN THIS PASS (0.99.73): wake what has just been wanted, put to sleep what has not been for 30 seconds. After the
    // commands (a window's report, a START, a preset) have landed, before the publish, so what the publish says is the plugins that exist.
    updatePluginLifecycle();
    scannerFrame();
    // THE AIRBAND MONITOR'S STEP, for the scanner's reason and in its place: a retune made this pass is already applied when it looks (stub
    // campaign B6). Inert while nothing listens.
    airbandFrame();
    // AUTOMATIC SQUELCH (stub campaign B12): after the monitor's step, so the channel levels its rule reads are this pass's. Inert while the switch is off.
    squelchAutoFrame();
}

double Engine::pumpPublish() {
    flushBookmarkSave(false);
    flushMarkerSave(false);
    // THE TRANSMITTER FOLLOWS THE RECEIVER'S DIAL (SPLIT off), once a pass,
    // after the pass's commands - the engine's own work since stage 4 (the
    // window called it after drawing its page; that is an interface reaching
    // into the engine). A no-op with no radio open, and a write only when
    // the frequency moved.
    followTransmitFrequency();
    // THE STATE FIRST, THEN THE STATUS LINES (the pump move's step P4; the survey's 1.6 C). The window takes the two as separate reads
    // - the lines, then the state - and the lines have no sequence of their own. With the pass on a thread of its own a frame can fall
    // between the two publishes of one pass: published the other way round it would read the lines of pass N with the state of pass
    // N-1 (a red source-error line beside a RUN flag that has not moved yet); published this way, and read in that order, the lines
    // can only be OLDER than the state, which is the lag every status line already had. test_look_order holds it with a hook in the gap.
    const double centre = publishReceiverState();
    if (testHooks_.betweenPublishes != nullptr) { testHooks_.betweenPublishes(); }
    // The status lines as this frame's commands left them, before any panel
    // draws (OPEN 3).
    publishStatusText();
    // The API's results, only now that the state they describe is published.
    api_->endPass();
    lastPublishNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                         std::memory_order_release);
    return centre;
}

Engine::PassStats Engine::passStats() const {
    PassStats s{passStats_.passes.load(), passStats_.sumUs.load(), passStats_.maxUs.load(), passStats_.overPeriod.load(), passStats_.wakes.load()};
    std::uint64_t h[PassHist::kBuckets];
    passDurHist_.copyTo(h);
    s.durationP50Us = PassHist::percentile(h, 50.0);
    s.durationP99Us = PassHist::percentile(h, 99.0);
    s.lagCount = passStats_.lagCount.load();
    s.lagSumUs = passStats_.lagSumUs.load();
    s.lagMaxUs = passStats_.lagMaxUs.load();
    s.kicks = passStats_.kicks.load();
    s.kicksDropped = passStats_.kicksDropped.load();
    s.kickedPasses = passStats_.kickedPasses.load();
    s.timedPasses = passStats_.timedPasses.load();
    passLagHist_.copyTo(h);
    s.lagP50Us = PassHist::percentile(h, 50.0);
    s.lagP99Us = PassHist::percentile(h, 99.0);
    return s;
}

void Engine::markPassStats() {
    std::lock_guard<std::mutex> lk(passMarkMutex_);
    passMark_.have = true;
    passMark_.atNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    passMark_.passes = passStats_.passes.load();
    passMark_.sumUs = passStats_.sumUs.load();
    passMark_.overPeriod = passStats_.overPeriod.load();
    passMark_.wakes = passStats_.wakes.load();
    passMark_.lagCount = passStats_.lagCount.load();
    passMark_.lagSumUs = passStats_.lagSumUs.load();
    passMark_.kicks = passStats_.kicks.load();
    passMark_.kicksDropped = passStats_.kicksDropped.load();
    passMark_.kickedPasses = passStats_.kickedPasses.load();
    passMark_.timedPasses = passStats_.timedPasses.load();
    passDurHist_.copyTo(passMark_.dur);
    passLagHist_.copyTo(passMark_.lag);
}

// The pass figures since the mark (or the thread's start), as the one JSON object the measurement result carries. The percentiles and the
// "at most" longest are of the WINDOW (the histograms differ bucket by bucket); maxUsSinceStart is the cumulative longest, for a reader who
// wants the stall that happened before the mark too.
std::string Engine::passStatsJson() const {
    PassMark m;
    {
        std::lock_guard<std::mutex> lk(passMarkMutex_);
        m = passMark_;
    }
    const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::uint64_t dur[PassHist::kBuckets], lag[PassHist::kBuckets];
    passDurHist_.copyTo(dur);
    passLagHist_.copyTo(lag);
    std::uint64_t durTop = 0, lagTop = 0;
    for (int i = 0; i < PassHist::kBuckets; ++i) {
        dur[i] -= m.dur[i];
        lag[i] -= m.lag[i];
        if (dur[i] > 0) { durTop = PassHist::upperEdge(i); }
        if (lag[i] > 0) { lagTop = PassHist::upperEdge(i); }
    }
    const std::uint64_t passes = passStats_.passes.load() - m.passes;
    const std::uint64_t sumUs = passStats_.sumUs.load() - m.sumUs;
    const std::uint64_t lagN = passStats_.lagCount.load() - m.lagCount;
    const std::uint64_t lagSum = passStats_.lagSumUs.load() - m.lagSumUs;
    const double seconds = m.have ? static_cast<double>(nowNs - m.atNs) / 1e9 : 0.0;
    char buf[1300];
    std::snprintf(buf, sizeof(buf),
                  "{\"marked\": %s, \"seconds\": %.3f, \"passes\": %llu, \"passesPerSecond\": %.2f, \"meanUs\": %.1f, \"p50UsAtMost\": %llu, "
                  "\"p99UsAtMost\": %llu, \"maxUsAtMost\": %llu, \"maxUsSinceStart\": %llu, \"overPeriod\": %llu, \"wakes\": %llu, "
                  "\"kicks\": %llu, \"kicksDropped\": %llu, \"kickedPasses\": %llu, \"timedPasses\": %llu, "
                  "\"submitToPublish\": {\"served\": %llu, \"meanUs\": %.1f, \"p50UsAtMost\": %llu, \"p99UsAtMost\": %llu, \"maxUsAtMost\": %llu}}",
                  m.have ? "true" : "false", seconds, static_cast<unsigned long long>(passes),
                  seconds > 0.0 ? static_cast<double>(passes) / seconds : 0.0,
                  passes > 0 ? static_cast<double>(sumUs) / static_cast<double>(passes) : 0.0,
                  static_cast<unsigned long long>(PassHist::percentile(dur, 50.0)), static_cast<unsigned long long>(PassHist::percentile(dur, 99.0)),
                  static_cast<unsigned long long>(durTop), static_cast<unsigned long long>(passStats_.maxUs.load()),
                  static_cast<unsigned long long>(passStats_.overPeriod.load() - m.overPeriod),
                  static_cast<unsigned long long>(passStats_.wakes.load() - m.wakes),
                  static_cast<unsigned long long>(passStats_.kicks.load() - m.kicks), static_cast<unsigned long long>(passStats_.kicksDropped.load() - m.kicksDropped),
                  static_cast<unsigned long long>(passStats_.kickedPasses.load() - m.kickedPasses),
                  static_cast<unsigned long long>(passStats_.timedPasses.load() - m.timedPasses), static_cast<unsigned long long>(lagN),
                  lagN > 0 ? static_cast<double>(lagSum) / static_cast<double>(lagN) : 0.0,
                  static_cast<unsigned long long>(PassHist::percentile(lag, 50.0)), static_cast<unsigned long long>(PassHist::percentile(lag, 99.0)),
                  static_cast<unsigned long long>(lagTop));
    return std::string(buf);
}


void Engine::pumpAudioMute() { updateAudioMute(); }

// --- THE TEARDOWN (engine/stage3b-pre, docs/engine-stage3.md OPEN 9) ---------
// Moved from AppWindow::run(), line for line and in the same order, with the
// reasons that went with each line.

void Engine::shutdownQuiesce() {
    // STAGE 5b: THE SERVERS STOP FIRST, before the control thread, as ~AppWindow stopped the ones it owned before anything else: a request in
    // flight must not reach a stopped pass, the servers' handlers read the snapshot and the API host, and nothing may be accepting a browser's
    // key while the keys are being let go below. WebServer::stop() can block for as long as the read timeout (10 s) when a client holds a
    // request open: here it blocks THE CALLER (the window's run, as ~AppWindow did before), never the pass - the control thread runs on until
    // the next step. A hard stop each - no thread of either server runs after it returns - and idempotent.
    servers_.shutdown();

    // STAGE 3b: the control thread is stopped next - it lets go of every key
    // at once, from this thread, and then joins (stopControlThread); every
    // step below runs on the engine's state with nothing beside it. A no-op
    // with no control thread (the window, today).
    stopControlThread();

    // THE LAST HEARD SECONDS of the Airband monitor go into the frequency list now, with the pass stopped, for the forced save that follows
    // (shutdownStop) to carry them (stub campaign B6).
    airbandShutdown();

    // THE TRANSMITTER BEFORE ANYTHING ELSE, AND IT IS NOT A STYLE CHOICE.
    // Everything else in this teardown can take its time; a radio that is
    // still keyed cannot. This is also the moment the dead-man's handle in
    // core/transmitter.hpp stops being hypothetical - from here on nothing
    // calls tick() again, so a transmitter that somehow survived this call
    // would be released by its own thread within kKeyAliveWait rather than
    // transmitting for the rest of the process's life.
    //
    // ITS COST IS CHARGED IN tests/test_shutdown_budget.cpp, as the only
    // column that ADDS to the source teardown rather than replacing it: a
    // Pluto transmitting while a SoapySDR receiver runs is two devices across
    // one teardown, which is the case the composition note there says makes
    // the sum the right arithmetic.
    transmitter_.stop();

    // THE GPS READER FIRST: its stop is bounded - one port read (200 ms)
    // while the worker is reading, GpsReader::kOpenAbandonWait (1 s, counted
    // in the shutdown budget) while it is still inside the port driver's
    // open, after which the worker is abandoned rather than joined - and it
    // has to precede the GL teardown, which is why it is not left to the
    // member destructor. Then one more poll, because a fix that arrived
    // during the last frame - or that a stop() just now let through, since a
    // fix that landed before the stop is kept - has not yet been applied, and
    // the save below is the one that persists it. This is what makes the
    // app-level test deterministic at a small frame count: the position is
    // on disk if the reader had it, whichever frame it arrived on.
    gpsReader_.stop();
    pollGpsReader();

    // Closing the window mid-take finalizes both recordings cleanly (same
    // contract as the toolbar Stop): taps out, then headers patched — before
    // the pipeline teardown below ends the sample flow they were taping.
    stopIqRecording();
    stopAudioRecording();
}

void Engine::shutdownStop() {
    flushBookmarkSave(true);
    flushMarkerSave(true);
    // Closing the window while receiving must not leave DSP threads pacing a
    // dead display; stop before teardown so the join happens while the object
    // graph is still fully alive. Not stopReceiver(): the takes were already
    // ended further up this teardown ("Closing the window mid-take"), and
    // this is the one pipeline_.stop() tests/test_stop_ends_recordings allows
    // outside it.
    pipeline_.stop();
}

void Engine::shutdownDrain(std::chrono::steady_clock::time_point drainDeadline) {
    // THE TWO LISTS' LAST SAVES (0.99.64) AND THE TWO TAKES' FILES (0.99.65) ARE DRAINED AGAINST THE SAME DEADLINE, the window's: taken before the
    // settings' last save was waited for, so what that wait leaves of it is what these get; they have been finishing since the stop, and the
    // shutdown budget is charged kSaveBound once. A default-constructed deadline waits for nothing and collects what has finished.
    drainListSaves(drainDeadline);
    drainRecordFinishes(drainDeadline);
}

void Engine::shutdownRelease() {
    // THE PATCH'S RADIOS STOP FIRST (0.99.17): each has a reader thread that
    // may be inside a plugin decoder, and each speaker's file is finalised
    // when its radio's sets go. The receiver's radio is not reopened on the
    // way out - the config already remembers it (patchMainKeep_).
    patchStopAll(false);
    detachAndUnloadPlugins();
}

void Engine::shutdown() {
    shutdownQuiesce();
    shutdownStop();
    shutdownRelease();
    shutdownDrain();
}

void Engine::pumpSourceHousekeeping() {
    // THE DEFERRAL'S ONE LOG LINE IS PER RADIO: it is written once while a scan is refused for the radio open, and
    // the next radio gets its own, so the latch is let go the moment the gate opens. The window used to send
    // FOXAPP_OP_CLEAR_STATUS (SOAPY_SCAN_DEFERRED_LOGGED) every frame the Source section was drawn with the gate
    // open (API 0.3 8.10); a pass knows the gate without anyone drawing anything.
    if (soapyScanDeferredLogged_ && !soapyScanGated()) { soapyScanDeferredLogged_ = false; }
    // THE SOUND CARD ROW IS SELECTED, SO THE CARDS ARE LISTED: once, on the worker, never while a list is being
    // taken or a card is opening, and not at all once they are listed - the test FOXAPP_OP_SOUND_CARDS_WANTED
    // applies. The window sent that every frame until the list arrived. (Unlike then it happens whether or not
    // the section is on screen: the selection is the engine's, and so is the wish.)
    if (sourceSel_ == kSoundCardRow && !soundCardListed_ && !soundCardScanPending_ && !soundCardOpenPending_) {
        scanSoundCards();
    }
}

void Engine::pumpWorkers() {
    pumpSourceHousekeeping();
    // A recording's file that finished opening on its worker is armed here, and one that finished closing releases the Record pressed behind
    // it (pollRecordStarts).
    pollRecordStarts(clockS());
    // ...and the disk's workers: an I/Q file's open, a frequency list's export, the patch Radio's recordings list (stub campaign B19).
    pollDiskJobs();
    pollSourceAsync();
    // ...and a sound card's list or open, the same way (app_window_soundcard.cpp).
    pollSoundCard();
    // ...and, once per frame, the one automatic reopen a radio whose driver
    // faulted gets (0.90.1). After the poll above, so a reopen that just
    // resolved is seen before this asks whether another is due.
    pollSoapyRecovery();
    // ...and the saved radio that was only busy at start-up (0.99.92): tried again at 2, 4, 8 and 16 s after each failure. One bool read on
    // every pass with nothing armed.
    pollSavedRadioRetry();
    // Release a wheel-burst retune the coalescer held back (~50 ms pacing).
    pollPendingRetune();
    // Same contract for the catalogue fetch / plugin download.
    pollPluginAsync();
    // ...and the web password's hash (stage 5a): a worker, never the pass.
    servers_.poll();
    // AFTER the poll, never before: the poll is what clears installPending_
    // when a transfer lands, and a pump that ran first would see the slot busy
    // and waste a frame on every module in the queue.
    pumpAddAll();
}

void Engine::pollSdrPlayTrouble() {
    const cascade::source::SdrPlayServiceTrouble trouble =
        cascade::source::sdrPlayServiceTrouble(cascade::source::processSdrPlayApi());
    if (trouble == sdrPlayTroubleSeen_) { return; }
    sdrPlayTroubleSeen_ = trouble;
    // THE TWO TROUBLES THAT ARE A RECOVERY, counted on the change to them: a scan whose worker was let go because the service never
    // answered, and a session the process gave up for good. (An Open that was refused is a radio failure and is counted as one where the
    // radio is opened.)
    if (trouble == cascade::source::SdrPlayServiceTrouble::EnumerationHung) {
        cascade::core::health::noteRecovered(cascade::core::health::Recovered::SdrEnum);
    } else if (trouble == cascade::source::SdrPlayServiceTrouble::SessionLost) {
        cascade::core::health::noteRecovered(cascade::core::health::Recovered::SdrLost);
    }
}

void Engine::pumpAudio() {
    // The diagnostics context a crash or a hang report prints, and the diagnostics bundle carries (window port group 14).
    pumpDiagContext();
    pollAudioOpen();
    pollAudioHealth();
    // ...and the samples the pipeline's own ring could not hold, once a minute
    // (0.99.86) - after the sink, but not gated on it.
    pollDspMinute();
    // The usage record's counts that need a look once a pass (the output's first play, a radio's first samples, the recoveries
    // nothing else may count), and the evidence of how the previous session ended.
    healthPoll();
    pollSdrPlayTrouble();
    exitCausePoll();
    pollMicOpen();
    // "Still running" beat, five-minute cadence. A no-op when reporting is
    // off, and never blocks - see HeartbeatSender::poll.
    telemetryHeartbeat_.poll(clockS());
    // THE CONFIG'S RECEIVER HALF, handed to the window once a frame (OPEN 2):
    // last, so it carries everything this frame's phases changed.
    publishConfig();
    // ...and the status lines, with the workers' answers of this frame
    // (OPEN 3) - drawn on the next.
    publishStatusText();
}

// The address the Transmit page opens on when nobody has typed one: the
// receiver's own Pluto, else the Pluto's factory address.
std::string Engine::transmitArgsSeed() const {
    return (sourceKind_ == "pluto" && !deviceArgs_.empty()) ? deviceArgs_ : std::string("uri=ip:192.168.2.1");
}

void Engine::publishStatusText() {
    StatusText t;
    t.sourceError = sourceError_;
    t.gpsRefusal = gpsRefusal_;
    t.catalogError = catalogError_;
    t.bandPlanError = bandPlanError_;
    t.tuneMismatchNote = tuneMismatchNote_;
    t.transmitError = transmitError_;
    t.soundCardMissing = soundCardMissing_;
    t.sdrPlayApiDetail = sdrPlayApiDetail_;
    t.sdrPlayAdvice = sdrPlayAdvice_;
    t.recordNotice = recordNotice_;
    t.recordError = recordError_;
    t.presetNote = presetNote_;
    t.pluginEnforceError = pluginEnforceError_;
    t.restoreKeepLabel = restoreKeepLabel_;
    t.audioHealth = audioHealthNote_;
    t.bookmarkNote = bookmarkImportNote_;
    t.bookmarkError = bookmarkError_;
    t.installReport = installReport_;
    t.installError = installError_;
    t.catalogStatus = catalogStatus_;
    t.addAllSummary = addAllSummary_;
    t.addAllFailed = addAllFailed_;
    // 8.10: the fault sentence whole (the pipeline's only while it is faulted - a healthy pass takes no lock).
    t.deviceFault = !testFault_.empty() ? testFault_ : (pipeline_.faulted() ? pipeline_.faultMessage() : std::string());
    std::lock_guard<std::mutex> lk(statusTextMutex_);
    statusText_ = std::move(t);
}

void Engine::setFrontEndFacts(FrontEndFacts f) {
    std::lock_guard<std::mutex> lk(frontEndFactsMutex_);
    frontEndFacts_ = std::move(f);
}

FrontEndFacts Engine::frontEndFacts() const {
    std::lock_guard<std::mutex> lk(frontEndFactsMutex_);
    return frontEndFacts_;
}

std::shared_ptr<const TrackFeed> Engine::trackFeed() const {
    std::lock_guard<std::mutex> lk(frontEndFactsMutex_);
    return frontEndFacts_.trackFeed;
}

// One track's enrichment from the handed-over answers (sorted by id, then kind): the same fields the window's enrichWebTrack hook
// set, and nothing for a track the plugin has not answered (infoState stays 0 = "no answer yet").
void Engine::applyTrackFeedInfo(const TrackFeed& x, cascade::net::RadioStatus::Track& w) {
    const auto it = std::lower_bound(x.tracks.begin(), x.tracks.end(), std::make_pair(w.id, w.kind),
                                     [](const TrackFeedInfo& a, const std::pair<std::string, unsigned>& key) {
                                         return a.id != key.first ? a.id < key.first : a.kind < key.second;
                                     });
    if (it == x.tracks.end() || it->id != w.id || it->kind != w.kind) { return; }
    w.infoState = it->infoState;
    if (it->infoState == 1u) {
        w.registration = it->registration;
        w.acType = it->acType;
        w.acOperator = it->acOperator;
        w.acCountry = it->acCountry;
    }
}

Engine::StatusText Engine::statusText() const {
    std::lock_guard<std::mutex> lk(statusTextMutex_);
    return statusText_;
}

void Engine::publishConfig() {
    cascade::core::AppConfig c = loadedCfg_;
    fillConfig(c);
    std::lock_guard<std::mutex> lk(configSnapMutex_);
    configSnap_ = std::move(c);
}

cascade::core::AppConfig Engine::configSnapshot() const {
    std::lock_guard<std::mutex> lk(configSnapMutex_);
    return configSnap_;
}

// EVERY STEP, IN THE FRAME'S ORDER, for a front end with no frame: the GPS
// read the window takes before its frame (run()), then the phases above.
// One drain, not two: the window's second drain is for its keyboard. The
// key is the engine API's: its pass (pumpFrameBegin) ticks the transmitter.
// --- THE CONTROL THREAD (stage 3b; engine.hpp) -------------------------------

void Engine::startControlThread() { startControlThread(ControlThreadOptions{}); }

void Engine::startControlThread(const ControlThreadOptions& options) {
    if (controlThread_.joinable()) { return; }
    txPluginStopping_.store(false, std::memory_order_seq_cst);   // a thread restarted after a stop may drive a plugin's key again (the window is closed)
    controlWatchdog_.start(options.watchdogReportDir, options.watchdogThresholdMs);
    {
        std::lock_guard<std::mutex> lk(controlExitMutex_);
        controlExited_ = false;
    }
    {
        // (The pacing state is the loop's and the stepping callers': set under the lock they wait on.)
        std::lock_guard<std::mutex> lk(controlWakeMutex_);
        controlWake_ = false;
        controlWakeNow_ = false;
        controlKick_ = false;
        lastKickNs_.store(0);
        passStartNs_.store(0);
        kickGapNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(options.kickGap).count());
        kickGraceNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(options.kickGrace).count());
        stepPending_ = false;
        // A step the last thread never finished (a stop mid-stepPass) must not leave the counters one behind for ever: the next request's
        // ticket would then never be reached.
        stepsDone_ = stepsAsked_;
        controlFloor_ = options.floor;
    }
    controlStepped_.store(options.stepped);
#ifdef _WIN32
    if (!options.stepped) {
        // THE PASS'S OWN TIMER (the capped-tail fix): made once, kept until ~Engine. CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is Windows 10
        // 1803+; without it the loop falls back to the condition variable and the process timer resolution it needs (below).
        if (passWakeEvent_.load() == nullptr) {
            if (HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr); ev != nullptr) { passWakeEvent_.store(ev); }
        }
        if (passTimer_.load() == nullptr) {
            constexpr DWORD kHighResolution = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
            if (HANDLE t = ::CreateWaitableTimerExW(nullptr, nullptr, kHighResolution, TIMER_ALL_ACCESS); t != nullptr) { passTimer_.store(t); }
        }
        if (passTimer_.load() != nullptr && passWakeEvent_.load() != nullptr) {
            (void)::ResetEvent(static_cast<HANDLE>(passWakeEvent_.load()));  // a wake from before this start is not one of its
            cascade::core::diagLogf("engine pass: waits on its own high-resolution timer; no process timer resolution requested");
        } else if (options.timerResolutionMs > 0 && timeBeginPeriod(options.timerResolutionMs) == TIMERR_NOERROR) {
            timerResolutionApplied_ = options.timerResolutionMs;
            cascade::core::diagWarnf("engine pass: no high-resolution timer here; falling back to a %u ms process timer resolution", options.timerResolutionMs);
        }
    }
#endif
    controlRun_.store(true);
    controlThread_ = std::thread(&Engine::controlLoop, this, options.period, options.floor, options.stepped);
}

void Engine::stopControlThread() {
    if (!controlThread_.joinable()) { return; }
    // EVERY KEY UP FIRST, from this thread, before the join - whatever the
    // control thread is doing, and however long its current pass takes: the
    // Transmitter's own key-up stores, and the remote's release. With nothing
    // asserting the key the TX thread plays it down by itself within a block
    // (core/transmitter.hpp). The key RULES are the control thread's: its
    // loop ends them as its last act (ApiHost::engineStopping), and a pass
    // still running cannot re-key - the rules drive the Transmitter only
    // from a session's key, and every session's key is ended there too.
    transmitter_.setLatched(false);
    transmitter_.setPttHeld(false);
    // A PLUGIN'S KEY GOES UP TOO, and a pass still running cannot ask for it again (stub campaign B13): the flag first, a seq_cst fence, then the request. txPluginStep does
    // the mirror image (its request, a seq_cst fence, then the flag), and the two fences are what make "either it sees the flag, or this false lands after its true" a
    // theorem and not a hope - the argument is written out at that function's load of the flag (engine_tx_plugin.cpp). The window itself is the pass's; the loop's last act
    // closes it (ApiHost::engineStopping).
    txPluginStopping_.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    transmitter_.setPluginKey(false);
    transmitter_.releaseRemote("the engine's control thread is stopping");
    controlRun_.store(false);
    controlParkForTest_.store(false);
    wakeControl();
    // A BOUNDED WAIT, THEN THE JOIN (kControlJoinWait says why it is joined
    // and not abandoned). The radio is already quiet: what is left is the
    // rest of the pass the thread is in, and a pass still running after the
    // bound becomes a line in the log rather than silence.
    bool exited = false;
    {
        std::unique_lock<std::mutex> lk(controlExitMutex_);
        exited = controlExitCv_.wait_for(lk, kControlJoinWait, [this] { return controlExited_; });
    }
    if (!exited) {
        cascade::core::diagWarnf("engine: the control thread did not come back within %lld ms; waiting for its pass's own bounds",
                  static_cast<long long>(kControlJoinWait.count()));
    }
    controlThread_.join();
    controlThreadId_.store(std::thread::id{});
    controlWatchdog_.stop();
#ifdef _WIN32
    if (timerResolutionApplied_ > 0) {
        (void)timeEndPeriod(timerResolutionApplied_);
        timerResolutionApplied_ = 0;
    }
#endif
}

// pump()'s phases, in pump()'s order, in the pass's three parts. The transmitter is ticked by the engine API's pass
// (pumpFrameBegin), from the key rules.
void Engine::controlPass() {
    runPassPart(PassPart::Front);
    runPassPart(PassPart::Mute);
    runPassPart(PassPart::Back);
}

void Engine::runPassPart(PassPart part) {
    switch (part) {
        case PassPart::Front:
            pollGpsReader();
            pumpFrameBegin();
            pumpInputs();
            pumpRemoteControls();
            pumpPlugins();
            pumpPatch();
            (void)pumpPublish();
            break;
        case PassPart::Mute:
            pumpAudioMute();
            break;
        case PassPart::Back:
            pumpWorkers();
            pumpAudio();
            break;
    }
}

// THE LOOP. Free-running (the product): a pass at least every `period`, at once when anything wakes it (a submit, a session's
// close, a posted piece of work, runPassNow), and never two passes' STARTS closer than `floor` (a wake inside the floor waits for the
// floor's end; a runPassNow's does not). Stepped (a bounded run): no pass of its own; the three parts of a pass, one for each
// stepPass, in the order the front end asks.
//
// WHY THE WAKE FLAG IS CLEARED WHEN THE PASS STARTS, not after the wait: a submit that lands while a pass is running sets the flag
// after the pass's own clearing, so the next pass is due at once (after the floor) - and a command pushed before the clearing is in
// the queue the pass drains afterwards. Clearing it after the wait would eat exactly that wake.
void Engine::controlLoop(std::chrono::milliseconds period, std::chrono::milliseconds floor, bool stepped) {
    controlThreadId_.store(std::this_thread::get_id());
    using SteadyClock = std::chrono::steady_clock;
    SteadyClock::time_point passStart{};
    bool first = true;
    std::int64_t wakeAtNs = 0;  // the wake the pass now running is serving (0: none - the period ran out, or a runPassNow)
    // THE WAIT, on the pass's own clock where there is one (see passTimer_): `lk` (controlWakeMutex_) is held on entry and on return, and
    // is dropped for the wait itself. It returns when `done()` (the flags, read under the lock) or `deadline`. Without the timer it is the
    // condition variable's wait_until, whose millisecond-granular timeout the PROCESS's timer resolution governs.
    void* const passTimer = stepped ? nullptr : passTimer_.load();
    void* const passEvent = stepped ? nullptr : passWakeEvent_.load();
    [[maybe_unused]] const bool ownClock = passTimer != nullptr && passEvent != nullptr;  // (used by the Windows branch only)
    auto waitUntil = [&](std::unique_lock<std::mutex>& lk, SteadyClock::time_point deadline, auto done) {
#ifdef _WIN32
        if (ownClock) {
            for (;;) {
                if (done()) { return; }
                const auto left = deadline - SteadyClock::now();
                if (left <= SteadyClock::duration::zero()) { return; }
                // A relative due time, in 100 ns units; SetWaitableTimer also un-signals a timer an earlier wait left expired.
                LARGE_INTEGER due;
                due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100));
                if (!::SetWaitableTimer(static_cast<HANDLE>(passTimer), &due, 0, nullptr, nullptr, FALSE)) {
                    controlWakeCv_.wait_until(lk, deadline, done);  // cannot arm it: the old wait, this once
                    return;
                }
                lk.unlock();
                const HANDLE handles[2] = {static_cast<HANDLE>(passTimer), static_cast<HANDLE>(passEvent)};
                (void)::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                lk.lock();
            }
        }
#endif
        controlWakeCv_.wait_until(lk, deadline, done);
    };
    while (controlRun_.load()) {
        controlWatchdog_.heartbeat();
        if (stepped) {
            // IDLE UNTIL ASKED: the heartbeat goes on while the front end draws (a wait of a quarter second at most), so a front
            // end that is simply busy is not a hung pass - a part that runs long is, and is the watchdog's to report.
            std::unique_lock<std::mutex> lk(controlWakeMutex_);
            while (controlRun_.load() && !stepPending_) {
                controlWakeCv_.wait_for(lk, kSteppedIdleBeat);
                controlWatchdog_.heartbeat();
            }
            if (!controlRun_.load()) { break; }
            const PassPart part = stepPart_;
            stepPending_ = false;
            if (part == PassPart::Front) { passesBegun_.fetch_add(1); }
            lk.unlock();
            if (part == PassPart::Front) {
                while (controlParkForTest_.load() && controlRun_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (const int stallMs = controlStallForTestMs_.exchange(0); stallMs > 0) {
                    controlInStallForTest_.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
                    controlInStallForTest_.store(false);
                }
            }
            if (!controlRun_.load()) { break; }
            runPassPart(part);
            lk.lock();
            if (part == PassPart::Back) {
                controlPasses_.fetch_add(1);
                passesEnded_.store(passesBegun_.load());
            }
            ++stepsDone_;
            lk.unlock();
            passDoneCv_.notify_all();
            continue;
        }
        {
            std::unique_lock<std::mutex> lk(controlWakeMutex_);
            const bool firstPass = first;
            if (!first) {
                // THE PASS PERIOD (engine-stage4.md decision 2): a pass every `period`, and at once when an interface submits or
                // closes a session (wakeControl) - so a control lands within one pass's own time of its submit, not a period later.
                //
                // THE FRAME'S KICK (6.9): while frames are kicking the pass, the timer is a fallback that must not fire before the next frame's
                // kick - a 60 Hz frame is 16.7 ms and the period 16 - so with a kick in the last `kickGrace` the wait runs to the kick's grace
                // (a kick resets it) rather than to the period; once the frames have stopped, the wait after the one grace that ran out is the
                // period again. Every kick arrives as a flag and ends the wait like a wake.
                const auto periodDone = [this] { return controlWake_ || controlWakeNow_ || controlKick_ || !controlRun_.load(); };
                const auto periodDeadline = [&] {
                    SteadyClock::time_point deadline = passStart + period;
                    if (const std::int64_t graceNs = kickGraceNs_.load(std::memory_order_relaxed); graceNs > 0) {
                        const std::int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count();
                        const std::int64_t kickNs = lastKickNs_.load(std::memory_order_relaxed);
                        if (kickNs != 0 && nowNs - kickNs < graceNs) {
                            deadline = std::max(deadline, SteadyClock::time_point(std::chrono::nanoseconds(kickNs + graceNs)));
                        }
                    }
                    return deadline;
                };
                for (;;) {
                    waitUntil(lk, periodDeadline(), periodDone);
                    if (periodDone()) { break; }
                    // THE TIMER RAN OUT - but the deadline this wait was given was fixed when it began, and a kick that arrived during it
                    // may have moved the fallback on: a kick the gap DROPPED (it came within kickGap of a pass the timer had just started)
                    // stamps lastKickNs_ and starts no pass, so without this look the timer fires again at the old deadline, the next
                    // kick again lands inside the gap of that pass, and the pass stays timer-paced for as many frames as it takes the two
                    // phases to drift apart (K1 measured up to 17 timer passes after the frames began; 6.9). The frames are flowing: wait on.
                    if (periodDeadline() <= SteadyClock::now()) { break; }
                }
                // THE FLOOR (adopted decision 6): a wake inside it waits the rest of it out; a runPassNow's does not.
                if (floor.count() > 0 && controlRun_.load() && !controlWakeNow_) {
                    waitUntil(lk, passStart + floor, [this] { return controlWakeNow_ || !controlRun_.load(); });
                }
            }
            first = false;
            if (controlWake_ || controlWakeNow_) { passStats_.wakes.fetch_add(1); }
            if (controlKick_) { passStats_.kickedPasses.fetch_add(1); }
            if (!firstPass && !controlWake_ && !controlWakeNow_ && !controlKick_ && controlRun_.load()) { passStats_.timedPasses.fetch_add(1); }
            // The pass's start is stamped here, with the flags cleared, under the lock frameKick judges its gap under.
            passStart = SteadyClock::now();
            passStartNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(passStart.time_since_epoch()).count(), std::memory_order_relaxed);
            controlWake_ = false;
            controlWakeNow_ = false;
            controlKick_ = false;
            wakeAtNs = controlWakeAtNs_;
            controlWakeAtNs_ = 0;
            if (!controlRun_.load()) { break; }
            passesBegun_.fetch_add(1);
        }
        if (testHooks_.passStarting != nullptr) { testHooks_.passStarting(); }
        controlWatchdog_.heartbeat();
        while (controlParkForTest_.load() && controlRun_.load()) {
            controlParkedForTest_.store(true);  // (the acknowledgement a test waits for: no pass is running, and none will begin)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        controlParkedForTest_.store(false);
        if (!controlRun_.load()) { break; }
        if (const int stallMs = controlStallForTestMs_.exchange(0); stallMs > 0) {
            controlInStallForTest_.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
            controlInStallForTest_.store(false);
        }
        controlPass();
        controlPasses_.fetch_add(1);
        {
            const std::uint64_t us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - passStart).count());
            passStats_.passes.fetch_add(1);
            passStats_.sumUs.fetch_add(us);
            passDurHist_.add(us);
            // SUBMIT TO PUBLISH: from the wake this pass serves to the moment its publish (results included) was done.
            if (wakeAtNs != 0) {
                const std::int64_t pubNs = lastPublishNs_.load(std::memory_order_acquire);
                if (pubNs >= wakeAtNs) {
                    const std::uint64_t lag = static_cast<std::uint64_t>((pubNs - wakeAtNs) / 1000);
                    passStats_.lagCount.fetch_add(1, std::memory_order_relaxed);
                    passStats_.lagSumUs.fetch_add(lag, std::memory_order_relaxed);
                    std::uint64_t lm = passStats_.lagMaxUs.load(std::memory_order_relaxed);
                    while (lag > lm && !passStats_.lagMaxUs.compare_exchange_weak(lm, lag)) {}
                    passLagHist_.add(lag);
                }
            }
            wakeAtNs = 0;
            std::uint64_t prev = passStats_.maxUs.load();
            while (us > prev && !passStats_.maxUs.compare_exchange_weak(prev, us)) {}
            if (us > static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(period).count())) {
                passStats_.overPeriod.fetch_add(1);
            }
        }
        {
            std::lock_guard<std::mutex> lk(controlWakeMutex_);
            passesEnded_.store(passesBegun_.load());
        }
        passDoneCv_.notify_all();
    }
    if (!stepped) {
        const PassStats st = passStats();
        cascade::core::diagLogf("engine pass: %llu passes, mean %.0f us, longest %.0f us, %llu longer than the %d ms period, %llu started by a wake",
                                static_cast<unsigned long long>(st.passes), st.passes > 0 ? static_cast<double>(st.sumUs) / static_cast<double>(st.passes) : 0.0,
                                static_cast<double>(st.maxUs), static_cast<unsigned long long>(st.overPeriod), static_cast<int>(period.count()),
                                static_cast<unsigned long long>(st.wakes));
        cascade::core::diagLogf("engine pass: duration p50 <= %llu us, p99 <= %llu us; submit to publish: %llu served, mean %.0f us, p50 <= %llu us, p99 <= %llu us, longest %llu us",
                                static_cast<unsigned long long>(st.durationP50Us), static_cast<unsigned long long>(st.durationP99Us),
                                static_cast<unsigned long long>(st.lagCount), st.lagCount > 0 ? static_cast<double>(st.lagSumUs) / static_cast<double>(st.lagCount) : 0.0,
                                static_cast<unsigned long long>(st.lagP50Us), static_cast<unsigned long long>(st.lagP99Us), static_cast<unsigned long long>(st.lagMaxUs));
    }
    // THE LAST ACT, by the thread that ticks the transmitter: stop() never
    // runs beside a tick(). The key is already up (stopControlThread let go
    // of it before asking this loop to end); this ends the TX thread. The key
    // rules first, on this thread (they are its), so nothing a pass decided
    // outlives the loop.
    api_->engineStopping();
    transmitter_.stop();
    {
        std::lock_guard<std::mutex> lk(controlExitMutex_);
        controlExited_ = true;
    }
    controlExitCv_.notify_all();
    // Anyone waiting for a pass or a step is told the thread has gone.
    passDoneCv_.notify_all();
}

// THE WAIT A FRONT-END THREAD MAKES FOR THE PASS (stepPass, runPassNow). `lk` holds controlWakeMutex_. If the caller is the front
// end's own thread - the one that installed the plugin UI's applier - it applies any directive the pass posts while it waits: the
// pass is, at that moment, doing what the frame's own apply would have answered (the unload handshake waits for the interface to
// acknowledge), and the interface's frame is blocked in this very call. The post notifies passDoneCv_ (PluginDirectiveLink's
// onPost), so the apply follows it at once; the 20 ms timeout is a safety net, not the mechanism.
bool Engine::waitForPassCounter(std::unique_lock<std::mutex>& lk, const std::function<bool()>& done,
                                std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    const bool frontEnd = pluginGuiThread_.load() == std::this_thread::get_id();
    for (;;) {
        if (done()) { return true; }
        if (!controlRun_.load()) { return false; }
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        if (frontEnd && pluginLink_.hasPending()) {
            std::shared_ptr<const PluginUiApplier> applier;
            {
                std::lock_guard<std::mutex> ak(pluginApplierMutex_);
                applier = pluginApplier_;
            }
            if (applier != nullptr) {
                lk.unlock();
                pluginLink_.applyNewest(*applier);
                lk.lock();
                continue;
            }
        }
        passDoneCv_.wait_for(lk, kPassWaitNet);
        if (frontEndWaitBeat_) { frontEndWaitBeat_(); }
    }
}

void Engine::stepPass(PassPart part) {
    if (!controlThreadRunning()) {
        runPassPart(part);  // no control thread: the front end is the pump, as it was before the move
        return;
    }
    if (!controlStepped_.load() || onControlThread()) { return; }  // free-running: the pass runs by itself
    // THE FRAME LOOP WAITS HERE FOR THE PART OF THE PASS THAT RAN ON ITS THREAD BEFORE THE SPLIT, so what the part does - a plugin reload, a recording's start - is the frame's time, and the
    // control thread may open the frame's scopes for the length of the wait (core/frame_timing.hpp, OwnedFrameScope). A thread that is not the frame loop's takes a loan that does nothing.
    cascade::core::FrameLoan frameLoan;
    // One requester at a time: the request slot holds one part.
    std::lock_guard<std::mutex> serial(stepRequestMutex_);
    std::unique_lock<std::mutex> lk(controlWakeMutex_);
    const std::uint64_t ticket = ++stepsAsked_;
    stepPart_ = part;
    stepPending_ = true;
    controlWakeCv_.notify_one();
    (void)waitForPassCounter(lk, [this, ticket] { return stepsDone_ >= ticket; }, std::chrono::milliseconds(60000));
}

bool Engine::runPassNow(std::chrono::milliseconds bound) {
    if (!controlThreadRunning()) {
        controlPass();
        return true;
    }
    if (onControlThread()) { return false; }  // a pass cannot wait for itself
    if (controlStepped_.load()) {
        const std::uint64_t target = passesBegun_.load() + 1;
        stepPass(PassPart::Front);
        stepPass(PassPart::Mute);
        stepPass(PassPart::Back);
        return passesEnded_.load() >= target;
    }
    std::unique_lock<std::mutex> lk(controlWakeMutex_);
    const std::uint64_t target = passesBegun_.load() + 1;
    controlWake_ = true;
    controlWakeNow_ = true;
    controlWakeCv_.notify_one();
    kickControlWait();
    return waitForPassCounter(lk, [this, target] { return passesEnded_.load() >= target; }, bound);
}

void Engine::pauseWaitWatchdog() {
    if (onControlThread()) {
        controlWatchdog_.pause();
    } else {
        host_->pauseWatchdog();
    }
}

void Engine::resumeWaitWatchdog() {
    if (onControlThread()) {
        controlWatchdog_.resume();
    } else {
        host_->resumeWatchdog();
    }
}

void Engine::pump() { controlPass(); }

}  // namespace cascade::engine
