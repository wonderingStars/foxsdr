// wire_a_recording.cpp - WIRING LANE wire-a, recording: daily folders (SET_RECORD_DAILY, fact.record.daily).
//
// DAILY FOLDERS (core/rec_daily.hpp, row 19): a take goes into <recordings folder>/<the day it starts>/... . The date is taken when the file is asked for (a take that crosses midnight stays where it began) in
// UTC (the default) or local time. The hook is the one place every take's place is decided (Engine::applyRecordName, engine_recname.cpp), called before the file-name template so that the template's own folders
// come below the day's. The folder is made by the model's containment-proving creator (no link, junction or file in the way, never outside the recordings folder); if it cannot be made the take keeps its
// default place and the log says so (with the reason, never the path). The default style is None: nothing changes until the user asks. The setting is kept in record-daily.json beside markers.json.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <limits>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/rec_daily.hpp"
#include "core/record_lock.hpp"
#include "core/status_clock.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"
#include "core/receiver_tables.hpp"

namespace cascade::engine {

namespace rd = cascade::core::recdaily;
namespace rl = cascade::core::reclock;

namespace {
bool parseDaily(const std::string& text, rd::Options& out, std::string& err) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "the text is not a JSON object"; return false; }
    rd::Options o;
    o.style = rd::Style::None;   // nothing changes until the user asks
    if (j.contains("style")) {
        if (!j["style"].is_number_integer()) { err = "the style is a whole number from 0 to 4"; return false; }
        const long long s = j["style"].get<long long>();
        if (s < 0 || s > 4) { err = "the style is a whole number from 0 to 4"; return false; }
        o.style = static_cast<rd::Style>(s);
    }
    if (j.contains("utc")) {
        if (!j["utc"].is_boolean()) { err = "utc is true or false"; return false; }
        o.clock = j["utc"].get<bool>() ? rd::Clock::Utc : rd::Clock::Local;
    }
    if (j.contains("template")) {
        if (!j["template"].is_string()) { err = "the template is text"; return false; }
        o.customTemplate = j["template"].get<std::string>();
    }
    // The model's own check of the result: a sample instant, the same rules as a take's.
    const long long sample = 1'790'000'000'000LL;
    const rd::Expanded e = rd::expandFolder(o, sample);
    if (!e.ok) { err = e.error; return false; }
    out = o;
    return true;
}
}  // namespace

std::string WireA::dailyJson() const {
    return nlohmann::json{{"style", static_cast<int>(daily_.style)}, {"utc", daily_.clock == rd::Clock::Utc}, {"template", daily_.customTemplate}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void WireA::dailyLoadOnce() {
    if (dailyLoaded_ || dir().empty()) { return; }
    dailyLoaded_ = true;
    const std::string text = readFile("record-daily.json");
    if (text.empty()) { return; }
    rd::Options o;
    std::string err;
    if (parseDaily(text, o, err)) {
        daily_ = o;
    } else {
        cascade::core::diagWarnf("record: record-daily.json was refused (%s); takes keep their default place", err.c_str());
    }
}

WireA::Result WireA::dailyOp(const FoxCommand& c, const std::string& text, Result res) {
    (void)c;
    dailyLoadOnce();
    rd::Options o;
    std::string err;
    if (!parseDaily(text, o, err)) { return refused(res, FOXAPI_BAD_ARGUMENT, err); }
    const rd::Options before = daily_;
    daily_ = o;
    const std::string now = dailyJson();
    daily_ = before;
    if (now == dailyJson()) { return refused(res, FOXAPI_NO_CHANGE, "that is in force already"); }
    daily_ = o;
    saveFile("record-daily.json", now + "\n");
    bump();
    return res;
}

void WireA::applyDaily(cascade::core::Recorder::OpenRequest& req) {
    dailyLoadOnce();
    if (daily_.style == rd::Style::None && daily_.customTemplate.empty()) { return; }
    const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rd::Options o = daily_;
    if (o.clock == rd::Clock::Local) { o.localOffsetMinutes = cascade::core::statusclock::systemUtcOffsetMinutes(nowMs); }
    const rd::Expanded e = rd::expandFolder(o, nowMs);
    if (!e.ok) {
        cascade::core::diagWarnf("record: the day's folder could not be named (%s); the take keeps its default place", e.error.c_str());
        return;
    }
    if (e.rel.empty()) { return; }
    std::error_code ec;
    const std::filesystem::path root(req.directory.empty() ? std::string(".") : req.directory);
    std::filesystem::create_directories(root, ec);   // (the recorder makes it anyway: it must exist for the containment proof)
    const rd::Ensured made = rd::ensureFolder(root, e.rel);
    if (!made.ok) {
        cascade::core::diagWarnf("record: the day's folder could not be made (%s); the take keeps its default place", made.error.c_str());
        return;
    }
    if (!req.path.empty()) { req.path = (made.dir / std::filesystem::path(req.path).filename()).string(); }
    req.directory = made.dir.string();
}

void WireA::recordingFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("record.daily", dailyJson());
    fact("record.container", container_ == 1 ? "flac" : "wav");
    fact("record.container.keep", containerKeep_ ? "1" : "0");
    fact("record.lock", lockPolicy_.lockWhileRecording ? "1" : "0");
    fact("record.lock.gain", lockPolicy_.lockGain ? "1" : "0");
    fact("record.lock.reason", lockReason_);
    for (std::size_t slot = 0; slot < 8; ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        std::string names;
        for (const rl::RecordLock::Entry& en : lock_.frozenSettings(static_cast<int>(slot))) {
            if (!names.empty()) { names += '\n'; }
            names += en.label;
        }
        fact("record.lock.frozen." + std::to_string(slot + 1), names);
    }
}

// ---- the lock while recording --------------------------------------------------------------------------------------------------------------------
// core/record_lock.hpp: what a take has frozen, and the sentence that says why. The engine's recorders are the truth: the model's slot for a radio is brought into line with them whenever a guarded
// command arrives (and once a pass), so a take started a moment ago is locked from the first command after it.

namespace rl = cascade::core::reclock;

void WireA::lockLoadOnce() {
    if (lockLoaded_ || dir().empty()) { return; }
    lockLoaded_ = true;
    const std::string text = readFile("record-lock.json");
    if (text.empty()) { return; }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return; }
    rl::Policy p;
    p.lockWhileRecording = j.value("lock", false);
    p.lockGain = j.value("gain", false);
    lockPolicy_ = p;
    lock_.setPolicy(p);
}

void WireA::lockSync(std::size_t slot) {
    if (slot >= 8 || !e_.receivers_[slot]) { return; }
    Receiver& r = *e_.receivers_[slot];
    // A take is open from the moment its file is being opened, not from the moment the engine collects the opened file and arms the recorder: the open runs on a
    // worker (RecordStart), the file is on disk while it is still out, and a retune that arrives in that window would change the centre the take is about to store.
    // starting() is true from the request until the open is collected or withdrawn (a Stop in the meantime ends it: nothing is held for a cancelled take).
    const bool iq = r.iqRecorder_.recording() || r.iqStart_.starting(), ifr = r.ifRecorder_.recording(), au = r.audioRecorder_.recording() || r.audioStart_.starting();
    const int radio = static_cast<int>(slot);
    if (!iq && !ifr && !au) {
        if (lock_.recording(radio)) { lock_.recordingStopped(radio); bump(); }
        return;
    }
    if (lock_.recording(radio)) { return; }   // frozen at the first look after the take started
    rl::Frozen f;
    f.centerHz = r.pipeline_.activeSource().centerFrequencyHz();
    f.sampleRateHz = r.pipeline_.inputRateHz();
    f.channelHz = r.pipeline_.activeSource().centerFrequencyHz() + r.pipeline_.vfoOffsetHz();
    f.channelBandwidthHz = r.vfoBandwidthHz_;
    f.channelMode = r.modeIndex_;
    f.sourceId = 0;
    f.audioRateHz = 0.0;
    const rl::RecordKind kind = iq ? rl::RecordKind::BasebandIq : (ifr ? rl::RecordKind::ChannelIf : rl::RecordKind::Audio);
    lock_.recordingStarted(radio, kind, f, false);
    bump();
}

bool WireA::recordGuard(const FoxCommand& c, std::string& why) {
    lockLoadOnce();
    rl::Setting what;
    double value = std::numeric_limits<double>::quiet_NaN();   // NaN is never "the same value": the request is judged a change
    const std::size_t slot = e_.scopedSlot();
    Receiver& r = e_.rx();
    switch (c.op) {
        case FOXAPI_OP_SET_CENTRE:
            what = rl::Setting::CenterFrequency;
            value = c.num[0];
            break;
        case FOXAPI_OP_SET_FREQUENCY:
            if (r.ifRecorder_.recording()) {
                what = rl::Setting::ChannelFrequency;
                value = c.num[0];
            } else if (r.ctun_) {
                return false;   // the VFO moves, the centre stays
            } else {
                what = rl::Setting::CenterFrequency;
                value = c.num[0] - r.pipeline_.vfoOffsetHz();
            }
            break;
        case FOXAPI_OP_SET_SAMPLE_RATE: what = rl::Setting::SampleRate; value = c.num[0]; break;
        case FOXAPI_OP_SELECT_SOURCE: what = rl::Setting::Source; break;
        case FOXAPP_OP_SET_PPM: what = rl::Setting::FrequencyCorrection; break;
        case FOXAPP_OP_SET_CONVERTER: what = rl::Setting::ConverterOffset; break;
        case FOXAPI_OP_RADIO_REMOVE: what = rl::Setting::CloseRadio; value = 1.0; break;
        case FOXAPI_OP_SET_GAIN: what = rl::Setting::Gain; break;
        case FOXAPI_OP_SET_MODE: what = rl::Setting::ChannelMode; value = static_cast<double>(c.ival[0] - 1); break;
        case FOXAPI_OP_SET_BANDWIDTH: what = rl::Setting::ChannelBandwidth; value = c.num[0]; break;
        default: return false;
    }
    lockSync(slot);
    const rl::Verdict v = lock_.check(static_cast<int>(slot), what, value);
    if (v.allowed) { return false; }
    why = v.reason;
    lockReason_ = v.reason;   // the whole sentence: the reply's message is short (FOXAPI_MESSAGE_CHARS)
    bump();
    return true;
}

WireA::Result WireA::lockOp(const FoxCommand& c, const std::string& text, Result res) {
    (void)text;
    lockLoadOnce();
    if ((c.ival[0] != 0 && c.ival[0] != 1) || (c.ival[1] != 0 && c.ival[1] != 1)) { return refused(res, FOXAPI_OUT_OF_RANGE, "the switches are 0 or 1"); }
    rl::Policy now;
    now.lockWhileRecording = c.ival[0] == 1;
    now.lockGain = c.ival[1] == 1;
    if (lockPolicy_.lockWhileRecording == now.lockWhileRecording && lockPolicy_.lockGain == now.lockGain) { return refused(res, FOXAPI_NO_CHANGE, "it is so already"); }
    lockPolicy_ = now;
    lock_.setPolicy(now);
    saveFile("record-lock.json", nlohmann::json{{"lock", now.lockWhileRecording}, {"gain", now.lockGain}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    bump();
    return res;
}

}  // namespace cascade::engine
