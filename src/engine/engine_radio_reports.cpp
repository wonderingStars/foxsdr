// engine_radio_reports.cpp - the engine's part of the radio reports (wave 8; core/radio_reporter.hpp and
// docs/multi-radio-records/radio-reports-app.md): which radio, when, and nothing else.
//
// WHAT THE ENGINE NEVER HANDS OVER: a frequency (the journal says a BAND by name), a bookmark, a file or recording name, a path, a position,
// anything decoded, a user or machine name. The call sites below pass the REQUESTED device (its driver key and arguments, from which the reporter
// reads only a model and a serial), a driver's own sentence (which the reporter scrubs) and numbers about the stream. core::radioreport's
// guard refuses the rest.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/receiver_tables.hpp"
#include "core/telemetry.hpp"
#include "core/version.hpp"

namespace cascade::engine {

namespace rr = cascade::core::radioreport;
namespace health = cascade::core::health;

namespace {

// A kind that is a physical radio (not the generator, a file or the patch's own sources).
bool physicalKind(const std::string& kind) { return !kind.empty() && kind != "siggen" && kind != "file"; }

long long rateSps(double hz) { return hz > 0.0 ? static_cast<long long>(std::llround(hz)) : -1; }

}  // namespace

rr::Identity Engine::radioReportIdentity() const {
    rr::Identity id;
    if (telemetryEnabled_ && !telemetryInstallId_.empty()) { id.installId = telemetryInstallId_; }
    id.version = cascade::versionString();
    id.os = cascade::core::osDescription();
    id.arch = cascade::core::archDescription();
    id.channel = cascade::core::installChannel();
    {
        std::lock_guard<std::mutex> lk(interfaceHalfMutex_);
        id.updateChannel = interfaceCfg_.updateChannel;
        id.diagOn = interfaceCfg_.diagnosticsEnabled;
    }
    id.window = telemetryWindow_;
    id.launches = telemetryLaunches_;
    return id;
}

void Engine::radioReportsStart(const cascade::core::AppConfig& cfg, const std::string& configDir) {
    (void)cfg;
    rr::Reporter::Options o;
    // No folder, no queue: a hermetic or bounded run (no config directory) queues and sends nothing.
    if (!configDir.empty()) { o.dir = configDir + "/radio-reports"; }
    o.endpoint = cascade::core::telemetryEndpoint();
    o.liveSend = false;   // armed by configureHeartbeat() for an interactive run, and then only if rr::kRadioReportsLiveSend is true
    o.identity = radioReportIdentity();
    rr::global().configure(o);
}

void Engine::radioReportsRefreshIdentity() {
    rr::Reporter& g = rr::global();
    const rr::Identity before = g.identity();
    const rr::Identity now = radioReportIdentity();
    g.setIdentity(now);
    if (before.installId != now.installId) { radioEnvSent_ = false; }
}

void Engine::noteWindowKind(const std::string& window) {
    telemetryWindow_ = (window == "new" || window == "legacy") ? window : std::string();
    radioReportsRefreshIdentity();
}

cascade::core::TelemetryReport Engine::telemetrySnapshot() {
    cascade::core::TelemetryReport r;
    r.installId = telemetryInstallId_;
    r.appVersion = cascade::versionString();
    r.os = cascade::core::osDescription();
    r.arch = cascade::core::archDescription();
    r.channel = cascade::core::installChannel();
    {
        std::lock_guard<std::mutex> halfLock(interfaceHalfMutex_);
        r.updateChannel = interfaceCfg_.updateChannel;   // "stable" or "beta"; toJson sends anything else empty
    }
    r.window = telemetryWindow_;
    r.synthetic = cascade::core::syntheticRun();
    r.firstRun = telemetryFirstRun_;
    r.firstVersion = telemetryFirstVersion_;
    r.launches = telemetryLaunches_;
    r.crashes = telemetryCrashes_;
    r.exits = telemetryExits_;
    r.stalls = stallLedger_->count();
    r.health = healthLedger_->encoded();
    const double now = host_->wallTimeS();
    r.session.seconds = static_cast<std::uint64_t>(now > telemetrySessionStart_ ? now - telemetrySessionStart_ : 0.0);
    r.session.modeSeconds = telemetryModeSeconds_;
    r.session.panels = telemetryPanels_;
    r.session.sdrModel = rx().deviceModel_;   // MODEL ONLY: sanitiseDevice stripped the serial
    r.session.radiosMax = telemetryRadiosMax_;
    for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
        if (p.loaded && r.session.plugins.size() < 20) { r.session.plugins.push_back(p.name + " " + p.version); }
    }
    return r;
}

// ---------------------------------------------------------------------------
// The failure of an open: ONE report, every time
// ---------------------------------------------------------------------------

void Engine::reportFailedOpen(const std::string& kind, const std::string& args, const std::string& reason, rr::Trigger trigger, bool superseded,
                              double requestRateHz, long long openMs) {
    if (!physicalKind(kind)) { return; }   // the generator and a recording are not radios that failed to open
    rr::FailureInput in;
    in.driverKind = kind;
    in.args = args;
    in.soundCard = kind == "soundcard";
    if (kind != "soapy" && !in.soundCard) { in.nativeLabel = nativeLabelFor(kind, args); }
    in.reason = reason;
    in.reasonClass = health::radioReasonWord(health::classifyRadioOpen(kind, reason));
    in.trigger = trigger;
    in.superseded = superseded;
    in.openMs = openMs;
    in.sampleRateSps = rateSps(requestRateHz);
    ++radioOpenFailures_;
    rr::Reporter& g = rr::global();
    {
        std::lock_guard<std::mutex> lk(interfaceHalfMutex_);
        g.setDiagOn(interfaceCfg_.diagnosticsEnabled);
    }
    g.fileFailure(in, cascade::core::DiagLog::instance().ringSnapshot());
    // a failure also puts the first session in front of the owner at once (contract 7), when this is the install's first
    if (telemetryLaunches_ == 1) { g.firstSession(telemetrySnapshot().toJson(), "on-failure"); }
}

// ---------------------------------------------------------------------------
// The journal
// ---------------------------------------------------------------------------

void Engine::journalOpenStart(const std::string& kind, const std::string& args, rr::Trigger trigger, double requestRateHz) {
    if (!physicalKind(kind)) { return; }
    rr::Event ev;
    ev.kind = "radio_open";
    ev.radio = kind == "soundcard" ? rr::soundCardIdentity() : rr::identify(kind, args, kind == "soapy" ? std::string() : nativeLabelFor(kind, args));
    ev.word = "start";
    ev.set("phase", "start").set("trigger", rr::triggerWord(trigger));
    if (requestRateHz > 0.0) { ev.set("sampleRateSps", rateSps(requestRateHz)); }
    // written to disk BEFORE the call that can hang or take the process down: an open that never comes back leaves a start and no result
    rr::global().event(ev);
}

void Engine::journalOpenOk(const std::string& kind, const std::string& args, rr::Trigger trigger, double requestRateHz, long long openMs) {
    if (!physicalKind(kind)) { return; }
    const std::size_t slot = scopedSlot();
    RadioJournalSlot& j = radioJournal_[slot];
    rr::RadioIdentity rid = kind == "soundcard" ? rr::soundCardIdentity() : rr::identify(kind, args, kind == "soapy" ? std::string() : nativeLabelFor(kind, args));
    j = RadioJournalSlot{};
    j.open = true;
    j.okSent = true;
    j.rid = rid;
    j.kind = kind;
    j.args = args;
    j.openedAtS = clockS();
    j.lastMinuteS = j.openedAtS;
    j.readAtOpen = rx().pipeline_.sourceSamplesRead();
    radioOpenedThisSession_ = true;
    rr::Event ev;
    ev.kind = "radio_open";
    ev.radio = rid;
    ev.word = "ok";
    ev.set("phase", "ok").set("trigger", rr::triggerWord(trigger));
    if (openMs >= 0) { ev.set("openMs", openMs); }
    if (requestRateHz > 0.0) { ev.set("sampleRateSps", rateSps(requestRateHz)); }
    const double centre = rx().pipeline_.activeSource().centerFrequencyHz();
    if (centre > 0.0) { ev.set("band", rr::bandName(centre)); }
    rr::global().event(ev);
}

void Engine::journalSettingsAtOpen(const std::string& kind, const std::string& args) {
    if (!physicalKind(kind)) { return; }
    rr::Event ev;
    ev.kind = "radio_settings";
    ev.radio = kind == "soundcard" ? rr::soundCardIdentity() : rr::identify(kind, args, kind == "soapy" ? std::string() : nativeLabelFor(kind, args));
    ev.word = "open";
    ev.set("why", "open");
    const double rate = rx().pipeline_.activeSource().sampleRateHz();
    if (rate > 0.0) { ev.set("sampleRateSps", rateSps(rate)); }
    if (rx().deviceAgcSupported_) { ev.set("gainMode", rx().deviceAgc_ ? "agc" : "manual"); }
    if (!rx().deviceAntenna_.empty()) { ev.text("antenna", rx().deviceAntenna_, 24); }
    const double centre = rx().pipeline_.activeSource().centerFrequencyHz();
    if (centre > 0.0) { ev.set("band", rr::bandName(centre)); }
    rr::global().event(ev);
}

void Engine::journalRecovery(const std::string& kind, const std::string& args, const char* phase, const std::string& what, const std::string& reason,
                             long long ms) {
    if (!physicalKind(kind)) { return; }
    rr::noteRecovery(kind, args, phase, what, reason, ms);
}

// ONCE A MINUTE PER OPEN RADIO (closeDspMinute calls it under the radio's own scope). The figures are the minute's: samples the source thread read,
// samples the ring dropped, the ring's high water. They are summed until the stream event's window (a minute, five after a radio's 500th of the day)
// has passed, and said with that window.
void Engine::journalStreamMinute(std::uint64_t samplesRead, std::uint64_t samplesDropped, std::size_t ringHighWaterSamples) {
    RadioJournalSlot& j = radioJournal_[scopedSlot()];
    if (!j.open) { return; }
    const double now = clockS();
    const double dt = std::max(1.0, now - j.lastMinuteS);
    j.lastMinuteS = now;
    j.accRead += samplesRead;
    j.accDropped += samplesDropped;
    j.accSec += dt;
    j.accHigh = std::max(j.accHigh, ringHighWaterSamples);
    const int window = rr::global().streamWindowSec(j.rid.key);
    if (j.accSec + 1.0 < static_cast<double>(window)) { return; }
    const long long seconds = std::clamp<long long>(static_cast<long long>(std::llround(j.accSec)), 1, 3600);
    rr::Event ev;
    ev.kind = "radio_stream";
    ev.radio = j.rid;
    ev.set("windowSec", seconds);
    ev.set("samplesPerSec", static_cast<long long>(std::llround(static_cast<double>(j.accRead) / static_cast<double>(seconds))));
    ev.set("drops", static_cast<long long>(std::min<std::uint64_t>(j.accDropped, 1000000000ull)));
    const double expected = rx().pipeline_.activeSource().sampleRateHz();
    if (expected > 0.0) { ev.set("expectedSps", rateSps(expected)); }
    const std::size_t cap = rx().pipeline_.ringCapacity();
    if (cap > 0) { ev.set("ringHighPct", static_cast<long long>(std::min<std::size_t>(100, j.accHigh * 100 / cap))); }
    ev.set("openSec", static_cast<long long>(std::clamp<double>(now - j.openedAtS, 0.0, 2592000.0)));
    rr::global().event(ev);
    j.accRead = 0;
    j.accDropped = 0;
    j.accSec = 0.0;
    j.accHigh = 0;
}

// ---------------------------------------------------------------------------
// radio_found: what a scan listed
// ---------------------------------------------------------------------------

void Engine::radioReportsScanLanded(bool native, bool soapy) {
    ++radioScans_;
    std::vector<std::string> models;
    auto note = [&models](const std::string& m) {
        if (!m.empty() && models.size() < 8 && std::find(models.begin(), models.end(), m) == models.end()) { models.push_back(m); }
    };
    rr::Reporter& g = rr::global();
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        if (!native) { break; }
        if (d.driver == cascade::gui::kPlutoDriverKey || d.driver == cascade::gui::kRtlTcpDriverKey) { continue; }   // an address row is not a radio that was found
        rr::Event ev;
        ev.kind = "radio_found";
        ev.radio = rr::identify(d.driver, d.args, d.label);
        ev.word = "usable";
        ev.set("binding", "usable").set("source", "scan");
        note(ev.radio.model);
        g.event(ev);
    }
    for (const cascade::usb::UsbDeviceInfo& u : nativeUnbound_) {
        if (!native) { break; }
        // PRESENT AND NOT REACHABLE (C3): the radio is on the bus and no driver FoxSDR can use is bound to it. The model is the bus's own description,
        // through the model scrub; the serial, when the descriptor gives one.
        rr::RadioIdentity rid;
        rid.driver = "other";
        rid.model = cascade::core::scrubModel(u.description.empty() ? std::string("USB radio") : u.description);
        if (rid.model.empty()) { rid.model = "other"; }
        rid.model = cascade::core::scrubModel(rr::clipUtf8(rid.model, 32));
        // the descriptor's serial, read the way a driver's is (a hardware serial; anything not shaped like one is left out)
        rid.serial = rr::serialFromArgs("rtlsdr", std::string("serial=") + u.serial);
        rid.key = rr::radioKey(rid.model, rid.serial);
        rr::Event ev;
        ev.kind = "radio_found";
        ev.radio = rid;
        ev.word = "wrong-driver";
        char usb[16];
        std::snprintf(usb, sizeof(usb), "%04x:%04x", static_cast<unsigned>(u.vid), static_cast<unsigned>(u.pid));
        ev.set("binding", "wrong-driver").set("source", "scan").set("usbId", usb);
        note(rid.model);
        g.event(ev);
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        if (!soapy) { break; }
        rr::Event ev;
        ev.kind = "radio_found";
        ev.radio = rr::identify("soapy", d.args);
        ev.word = "usable";
        ev.set("binding", "usable").set("source", "scan");
        note(ev.radio.model);
        g.event(ev);
    }
    for (const std::string& m : models) { if (std::find(radioScanModels_.begin(), radioScanModels_.end(), m) == radioScanModels_.end() && radioScanModels_.size() < 8) { radioScanModels_.push_back(m); } }
}

// ---------------------------------------------------------------------------
// The pass: session_env, the first session, every slot's state, the beat's radios
// ---------------------------------------------------------------------------

void Engine::radioReportsPass() {
    rr::Reporter& g = rr::global();
    const double now = clockS();

    // THE MACHINE, once a session: read on a worker a few seconds in (the window has said which it is by then), sent as one event.
    if (!radioEnvSent_ && !radioEnvPending_ && now >= 3.0 && g.active() && !g.identity().installId.empty()) {
        radioEnvFuture_ = std::async(std::launch::async, []() { return cascade::core::readSessionEnv(); });
        radioEnvPending_ = true;
    }
    if (radioEnvPending_ && radioEnvFuture_.valid() && radioEnvFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const cascade::core::SessionEnv env = radioEnvFuture_.get();
        radioEnvPending_ = false;
        radioEnvSent_ = true;
        rr::Event ev;
        ev.kind = "session_env";
        ev.text("cpu", env.cpu.empty() ? std::string("unknown") : env.cpu, 64);
        if (env.cpuThreads > 0) { ev.set("cpuThreads", env.cpuThreads); }
        if (env.ramMb > 0) { ev.set("ramMb", static_cast<long long>(std::min<std::uint64_t>(env.ramMb, 4194304ull))); }
        if (!env.gpu.empty()) { ev.text("gpu", env.gpu, 64); }
        if (!env.gpuDriver.empty()) { ev.set("gpuDriver", env.gpuDriver); }
        if (!env.usbHosts.empty()) { ev.fields.emplace_back("usbHosts", rr::FieldValue(env.usbHosts)); }
        if (env.displayCount > 0) { ev.set("displayCount", env.displayCount); }
        if (!env.displayScalePct.empty()) { ev.set("displayScalePct", env.displayScalePct); }
        const std::string api = rx().pipeline_.audio().openedHostApi();
        ev.set("audioApi", api.empty() ? std::string("none") : std::string(health::audioApiWord(health::audioApiFromName(api))));
        g.event(ev);
    }

    // THE FIRST SESSION, about a minute in (contract 7): only in the install's first launch; the reporter enforces the rest
    if (!radioFirstSessionSent_ && telemetryLaunches_ == 1 && now >= 60.0 && g.active()) {
        radioFirstSessionSent_ = true;
        g.firstSession(telemetrySnapshot().toJson(), "after-minute");
    }

    // EVERY SLOT: a radio that closed, one that opened by a path that did not say so, a fault, and "opened and no samples"
    std::vector<std::string> keys;
    for (std::size_t s = 0; s < kMaxReceivers; ++s) {
        if (!receivers_[s]) { continue; }
        SlotScope scope(this, s);
        RadioJournalSlot& j = radioJournal_[s];
        const std::string kind = rx().sourceKind_;
        const bool hw = rx().device_ != nullptr && physicalKind(kind);
        rr::RadioIdentity rid;
        if (hw) { rid = kind == "soundcard" ? rr::soundCardIdentity() : rr::identify(kind, rx().deviceArgs_, kind == "soapy" ? std::string() : nativeLabelFor(kind, rx().deviceArgs_)); }
        if (j.open && (!hw || rid.key != j.rid.key)) {
            rr::Event ev;
            ev.kind = "radio_close";
            ev.radio = j.rid;
            const char* cause = j.faulted ? "died" : (radioQuitting_ ? "quit" : (hw ? "switch-radio" : "user"));
            ev.word = cause;
            ev.set("cause", cause).set("openSec", static_cast<long long>(std::clamp<double>(now - j.openedAtS, 0.0, 2592000.0)));
            if (j.faulted && !j.faultReason.empty()) { ev.text("reason", j.faultReason, 400); }
            ev.urgent = j.faulted;
            g.event(ev);
            j = RadioJournalSlot{};
        }
        if (hw && !j.open) {
            // opened by a path that did not say so (a plugin's radio, a restore that raised no event): state only, no invented trigger
            j.open = true;
            j.rid = rid;
            j.kind = kind;
            j.args = rx().deviceArgs_;
            j.openedAtS = now;
            j.lastMinuteS = now;
            j.readAtOpen = rx().pipeline_.sourceSamplesRead();
            radioOpenedThisSession_ = true;
        }
        if (hw && j.open) {
            if (!j.faulted && rx().pipeline_.faulted()) {
                j.faulted = true;
                j.faultReason = rx().pipeline_.faultMessage();
            }
            // OPENED AND NO SAMPLES (L3, the survey's item 2): twenty seconds after the open and the source thread has read nothing
            if (!j.noSamplesDone && now - j.openedAtS >= 20.0) {
                j.noSamplesDone = true;
                if (rx().pipeline_.sourceSamplesRead() == j.readAtOpen) {
                    rr::Event ev;
                    ev.kind = "radio_stream";
                    ev.radio = j.rid;
                    ev.set("windowSec", 20).set("samplesPerSec", 0).set("drops", 0).set("openSec", 20);
                    const double expected = rx().pipeline_.activeSource().sampleRateHz();
                    if (expected > 0.0) { ev.set("expectedSps", rateSps(expected)); }
                    ev.urgent = true;
                    g.event(ev);
                }
            }
            keys.push_back(j.rid.key);
        }
    }
    // the beat names the radios that are open right now
    telemetryHeartbeat_.setRadios(keys);
    g.setOpenRadios(keys);
    g.poll();
}

// THE FIRST STEP OF THE TEARDOWN: every open radio closes for `quit`, a session that never opened a radio says so with what its scan listed, and
// the queue gets one bounded last send (quit is never delayed by it: the transfer is abortable and the wait is short).
void Engine::radioReportsQuit() {
    rr::Reporter& g = rr::global();
    if (!g.active()) { return; }
    radioQuitting_ = true;
    const double now = clockS();
    for (std::size_t s = 0; s < kMaxReceivers; ++s) {
        RadioJournalSlot& j = radioJournal_[s];
        if (!j.open) { continue; }
        rr::Event ev;
        ev.kind = "radio_close";
        ev.radio = j.rid;
        const char* cause = j.faulted ? "died" : "quit";
        ev.word = cause;
        ev.set("cause", cause).set("openSec", static_cast<long long>(std::clamp<double>(now - j.openedAtS, 0.0, 2592000.0)));
        if (j.faulted && !j.faultReason.empty()) { ev.text("reason", j.faultReason, 400); }
        g.event(ev);
        j.open = false;
    }
    if (!radioOpenedThisSession_ && !radioSessionEndSent_) {
        // "A SESSION ENDED WITH NO RADIO OPENED" (L1): what the scan listed (models only), how many scans landed, and how many opens failed.
        radioSessionEndSent_ = true;
        rr::Event ev;
        ev.kind = "session_end";
        ev.set("opened", false).set("scans", static_cast<long long>(std::min<std::uint64_t>(radioScans_, 1000000ull)));
        ev.set("openFailures", static_cast<long long>(std::min<std::uint64_t>(radioOpenFailures_, 1000000ull)));
        ev.set("listed", radioScanModels_);
        ev.set("sessionSec", static_cast<long long>(std::clamp<double>(now, 0.0, 2592000.0)));
        g.event(ev);
    }
    g.flushAtQuit(1500);
}

}  // namespace cascade::engine
