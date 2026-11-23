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
#include <map>
#include <string>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/radio_events.hpp"
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
    id.synthetic = cascade::core::syntheticRun();   // a test, CI or the owner's own run: the Worker drops every message of it (contract 1.3)
    return id;
}

void Engine::radioReportsStart(const cascade::core::AppConfig& cfg, const std::string& configDir) {
    (void)cfg;
    rr::Reporter::Options o;
    // No folder, no queue: a hermetic or bounded run (no config directory) queues and sends nothing.
    if (!configDir.empty()) { o.dir = configDir + "/radio-reports"; }
    o.endpoint = cascade::core::telemetryEndpoint();
    o.liveSend = false;   // armed by configureHeartbeat() for an interactive run, and only if rr::kRadioReportsLiveSend is true (it is, from 0.99.109)
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
                              double requestRateHz, long long openMs, bool seriesRetry) {
    if (!physicalKind(kind)) { return; }   // the generator and a recording are not radios that failed to open
    rr::FailureInput in;
    in.driverKind = kind;
    in.args = args;
    in.soundCard = kind == "soundcard";
    if (kind != "soapy" && !in.soundCard) { in.nativeLabel = nativeLabelFor(kind, args); }
    in.reason = reason;
    in.reasonClass = health::radioReportClassWord(kind, reason);
    in.trigger = trigger;
    in.superseded = superseded;
    in.openMs = openMs;
    in.sampleRateSps = rateSps(requestRateHz);
    in.countOnly = seriesRetry;
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
    // A RADIO STILL HELD BY THIS SLOT was replaced before the pass noticed (the open is close-first and can finish inside one pass): it closed for `switch-radio`
    // (or `died`, when its fault was seen).
    if (j.open) { journalCloseEvent(j, j.faulted ? "died" : "switch-radio", clockS()); }
    j = RadioJournalSlot{};
    j.open = true;
    j.okSent = true;
    j.rid = rid;
    j.kind = kind;
    j.args = args;
    j.openedAtS = clockS();
    j.lastMinuteS = j.openedAtS;
    (*radioRun_)[slot] = RadioRun{};   // the previous radio's running seconds are not this one's (review of 3813659b, S4)
    j.readAtOpen = rx().pipeline_.sourceSamplesRead();
    radioOpenedThisSession_ = true;
    rr::registerSource(rx().device_, rid);   // the drivers' own events (a refused setting, a radio that went away) find their radio through this
    rr::Event ev;
    ev.kind = "radio_open";
    ev.radio = rid;
    ev.word = "ok";
    ev.set("phase", "ok").set("trigger", rr::triggerWord(trigger));
    if (openMs >= 0) { ev.set("openMs", openMs); }
    if (requestRateHz > 0.0) { ev.set("sampleRateSps", rateSps(requestRateHz)); }
    const double centre = rx().pipeline_.activeSource().centerFrequencyHz();
    if (centre > 0.0) { ev.set("band", rr::bandName(centre)); }
    // the firmware the radio itself reported at open (the Airspy HF+ is the one driver that hands it over; nothing is probed for the others)
    if (const auto* hf = dynamic_cast<const cascade::source::AirspyHfSource*>(rx().device_)) {
        const std::string fw = rr::versionText(hf->firmwareVersion());
        if (!fw.empty()) { ev.set("firmware", fw); }
    }
    rr::global().event(ev);
}

// AN OPEN THAT SUCCEEDED AND WAS DROPPED because the user had moved on to something else (asyncOpenStillWanted): not a fault of the radio, so no failure
// report; the journal says it opened and closed again, so the site's account of the radio has no hole where a hardware open happened.
void Engine::journalOpenSuperseded(const std::string& kind, const std::string& args, rr::Trigger trigger, double requestRateHz, long long openMs) {
    if (!physicalKind(kind)) { return; }
    const rr::RadioIdentity rid = kind == "soundcard" ? rr::soundCardIdentity() : rr::identify(kind, args, kind == "soapy" ? std::string() : nativeLabelFor(kind, args));
    rr::Event ok;
    ok.kind = "radio_open";
    ok.radio = rid;
    ok.word = "ok";
    ok.set("phase", "ok").set("trigger", rr::triggerWord(trigger));
    if (openMs >= 0) { ok.set("openMs", openMs); }
    if (requestRateHz > 0.0) { ok.set("sampleRateSps", rateSps(requestRateHz)); }
    rr::global().event(ok);
    rr::Event closed;
    closed.kind = "radio_close";
    closed.radio = rid;
    closed.word = "switch-radio";
    closed.set("cause", "switch-radio").set("openSec", 0);
    rr::global().event(closed);
}

void Engine::journalCloseEvent(const RadioJournalSlot& j, const char* cause, double now) {
    // a driver that saw the radio go away said so at the place it saw it (rr::noteDeviceFault): its event is this radio's ending, not a second one
    const bool saidByDriver = rr::takeCloseReported(j.rid.key);
    rr::forgetRadio(j.rid.key);
    if (saidByDriver) { return; }
    // the pipeline's latch ("died") says the radio ended; the sentence and the driver's own verdict say HOW: gone from the bus, given up on, or an error
    std::string why = cause;
    if (j.faulted && why == "died") {
        const std::string refined = rr::closeCauseFor(j.faultDead, j.faultReason);
        if (!refined.empty()) { why = refined; }
    }
    rr::Event ev;
    ev.kind = "radio_close";
    ev.radio = j.rid;
    ev.word = why;
    ev.set("cause", why).set("openSec", static_cast<long long>(std::clamp<double>(now - j.openedAtS, 0.0, 2592000.0)));
    if (j.faulted && !j.faultReason.empty()) { ev.text("reason", j.faultReason, 400); }
    ev.urgent = j.faulted;
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
// THE WINDOW'S ORIGIN IS ONE (review of 70d16f00, finding 4): the figures are the DSP minute's, taken from the marks that began at `beganS` (the
// minute clock's arming, or the previous close), so the window's length is measured from the same instant - not from the radio's open, which
// understated the first window's rate by the whole start-up. A radio opened INSIDE the minute counts from its open, with the samples read
// since it (readNow - readAtOpen), so the pre-open generator's samples are not credited to it either.
void Engine::journalStreamMinute(std::uint64_t samplesRead, std::uint64_t samplesDropped, std::uint64_t samplesAbsent, std::size_t ringHighWaterSamples,
                                 double beganS, std::uint64_t readNow) {
    RadioJournalSlot& j = radioJournal_[scopedSlot()];
    // A FAULTED RADIO IS OVER (field data 2026-11-19: 38 windows after a close): its slot stays open until the pass sees it leave, but no window is said for it
    if (!j.open) { return; }
    // ...UNLESS IT STREAMS AGAIN (final look of 7421b4c6, F5): a radio whose fault the pipeline has since cleared (a restart after a stream fault)
    // and which delivered samples this minute is in use, not over. The pass re-latches j.faulted if the pipeline faults again.
    if (j.faulted) {
        if (samplesRead == 0 || rx().pipeline_.faulted()) { return; }
        j.faulted = false;
        j.faultReason.clear();
        j.faultDead = false;
    }
    const double now = clockS();
    // FAULT E (field data 2026-11-23: one RSP1B "open" with 61 zero-sample minutes of 69, an RSPduo 31 of 105): a radio
    // that is open while the receiver is STOPPED is not delivering anything, by the user's own choice, and the
    // source thread's 5 s stall watchdog (which runs only on reads) rightly says nothing. Those minutes were counted
    // as silent streams. A stopped receiver says no stream window; the clock moves on so the next running window is its own.
    // (review of 885ebd7e: "running" is the seconds the pass saw the pipeline running in this minute, not the state at the close, so a silent
    // stream the user stopped inside the minute is still said)
    RadioRun& run = (*radioRun_)[scopedSlot()];
    const double ranSec = run.runningSec;
    run.runningSec = 0.0;
    // THE PREVIOUS SOURCE'S SAMPLES ARE NOT THIS RADIO'S (final look of 7421b4c6, F6b): the read mark is the receiver's and the pipeline's counter runs
    // across sources, so the open's first window holds whatever was read between the last mark and the open. j.readAtOpen is the counter at the
    // open; readNow is the counter at this close, so the mark this window started from is readNow minus what this window read (closeDspMinute
    // passes the very counter it has just moved the minute's mark to).
    if (samplesRead > 0 && readNow >= samplesRead) {
        const std::uint64_t windowStartMark = readNow - samplesRead;
        if (j.readAtOpen > windowStartMark) { samplesRead -= std::min<std::uint64_t>(samplesRead, j.readAtOpen - windowStartMark); }
    }
    // THE WINDOW'S ORIGIN (review of 70d16f00, finding 4): where the minute's marks began (beganS: the arming or the previous close), or the radio's
    // own open when that came later.
    const double origin = std::max(beganS, j.openedAtS);
    const double wall = std::max(1.0, now - origin);
    if (samplesRead == 0 && ranSec < 0.5 * wall) {
        j.lastMinuteS = now;
        return;
    }
    // THE MINUTE'S LENGTH IS THE TIME THE PIPELINE RAN IN IT (F6a), where the pass saw it run: a minute the receiver ran 20 s of is not 60 s of
    // rate. The window's own length where the pass saw none (the seams that call this directly).
    const double dt = ranSec > 0.0 ? std::clamp(ranSec, 1.0, wall) : wall;
    j.lastMinuteS = now;
    j.accRead += samplesRead;
    j.accDropped += samplesDropped;
    j.accAbsent += samplesAbsent;
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
    // THE APPLICATION'S OWN PAUSE, under its own name: samples the ring could not hold while the DSP thread was deliberately absent (bounded to
    // 2 s an absence), never folded into `drops` and never left out.
    if (j.accAbsent > 0) { ev.set("absentDrops", static_cast<long long>(std::min<std::uint64_t>(j.accAbsent, 1000000000ull))); }
    const double expected = rx().pipeline_.activeSource().sampleRateHz();
    if (expected > 0.0) { ev.set("expectedSps", rateSps(expected)); }
    const std::size_t cap = rx().pipeline_.ringCapacity();
    if (cap > 0) { ev.set("ringHighPct", static_cast<long long>(std::min<std::size_t>(100, j.accHigh * 100 / cap))); }
    ev.set("openSec", static_cast<long long>(std::clamp<double>(now - j.openedAtS, 0.0, 2592000.0)));
    rr::global().event(ev);
    j.accRead = 0;
    j.accDropped = 0;
    j.accAbsent = 0;
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
    // ONE PHYSICAL RADIO IS ONE radio_found PER SCAN (field data 2026-11-19: two RSPduos seen through SoapySDR became eight phantom rows "Dev0".."Dev7", and an RSP1
    // also arrived under the mirisdr driver): a finding whose serial a driver already listed in this scan is the same radio seen by another driver, and is not said again.
    std::map<std::string, std::string> serialSeenBy;
    auto repeated = [&serialSeenBy](const rr::RadioIdentity& rid, const char* route) {
        if (rid.serial.empty()) { return false; }
        const auto it = serialSeenBy.find(rid.serial);
        const std::string who = std::string(route) + "/" + rid.driver;   // the route (native or SoapySDR) and the driver word
        if (it == serialSeenBy.end()) { serialSeenBy[rid.serial] = who; return false; }
        return it->second != who;
    };
    for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
        if (!native) { break; }
        if (d.driver == cascade::gui::kPlutoDriverKey || d.driver == cascade::gui::kRtlTcpDriverKey) { continue; }   // an address row is not a radio that was found
        rr::Event ev;
        ev.kind = "radio_found";
        ev.radio = rr::identify(d.driver, d.args, d.label);
        if (repeated(ev.radio, "native")) { continue; }
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
    if (soapy && !native) {
        // the two scans land separately: the native list of the scan before is what a SoapySDR finding is compared with
        for (const cascade::source::NativeDeviceInfo& d : nativeDevices_) {
            if (d.driver == cascade::gui::kPlutoDriverKey || d.driver == cascade::gui::kRtlTcpDriverKey) { continue; }
            repeated(rr::identify(d.driver, d.args, d.label), "native");
        }
    }
    for (const cascade::source::SoapyDeviceInfo& d : soapyDevices_) {
        if (!soapy) { break; }
        rr::rememberSoapyModel(d.args, rr::soapyDeviceModel(d.args));   // so a radio opened later by saved arguments with no `product` is named by its own model
        rr::Event ev;
        ev.kind = "radio_found";
        ev.radio = rr::identify("soapy", d.args);
        if (repeated(ev.radio, "soapy")) { continue; }
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
        if (hw) {
            // the same radio as last pass: its identity is not worked out again sixty times a second
            if (j.open && j.kind == kind && j.args == rx().deviceArgs_) {
                rid = j.rid;
            } else {
                rid = kind == "soundcard" ? rr::soundCardIdentity() : rr::identify(kind, rx().deviceArgs_, kind == "soapy" ? std::string() : nativeLabelFor(kind, rx().deviceArgs_));
            }
        }
        if (j.open && (!hw || rid.key != j.rid.key)) {
            journalCloseEvent(j, j.faulted ? "died" : (radioQuitting_ ? "quit" : (hw ? "switch-radio" : "user")), now);
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
            (*radioRun_)[s] = RadioRun{};
            j.readAtOpen = rx().pipeline_.sourceSamplesRead();
            radioOpenedThisSession_ = true;
            rr::registerSource(rx().device_, rid);
        }
        if (hw && j.open) {
            {
                RadioRun& run = (*radioRun_)[scopedSlot()];
                const double sincePass = run.lastPassS > 0.0 ? std::min(now - run.lastPassS, 5.0) : 0.0;
                run.lastPassS = now;
                if (rx().pipeline_.running()) { run.runningSec += std::max(0.0, sincePass); }
            }
            if (!j.faulted && rx().pipeline_.faulted()) {
                j.faulted = true;
                j.faultReason = rx().pipeline_.faultMessage();
                j.faultDead = rx().device_ != nullptr && rx().device_->deviceDead();
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
        journalCloseEvent(j, j.faulted ? "died" : "quit", now);
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
