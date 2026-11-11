// engine_schedule.cpp - TIMED RECORDINGS, the engine's side (docs/API.md 6.23; the pure clock is core/schedule.*).
//
// WHAT THIS DOES. A list of entries ("radio 2, at 14:32 UTC, for 12 minutes, 137.9 MHz NFM, record the I/Q") is kept in the settings. Once a pass the
// clock (core::sched::Runner) says which entry starts, which ends, which was missed; this file turns that into the same engine commands a person would
// give: tune (SET_FREQUENCY, SET_MODE, SET_BANDWIDTH) and record (RECORD_AUDIO, RECORD_IQ) on that radio, and the same commands the other way at the end -
// with the old tuning put back when the entry says so. NOTHING IS DONE BEHIND THE PASS'S BACK: every action is applyCommand on the pass thread, and the
// recorder's file is opened on its worker, never here (disk work is never on the pass).
//
// WHAT IT NEVER DOES. It never takes over a radio: an entry whose radio is closed, stopped, still opening, already recording (by hand or for another entry)
// or held by the scanner is REFUSED, in words (fact.schedule.entry.<id>), and the occurrence is used up. It never starts anything late (30 s at most; see
// the clock), never starts a missed entry after the fact, and never runs while FoxSDR is closed - a start that fell in the dark is counted and shown.
//
// THE CLOCK. Unix milliseconds from the system clock, or from the test hook "test.scheduleClock=<unix ms>" (an engine made with testHooks=1 only; 0 gives
// the real one back), so a test sets the time and steps it without sleeping.
//
// A FAILED OPEN IS FOUND OUT, not assumed away: a recorder opens its file on a worker, so a folder that cannot be written or a disk that is full answers a
// few passes after the command. The entry is "starting" until the recorder reports it is recording; if the open ended with an error instead, the entry is
// REFUSED with the recorder's own sentence, the other recorder (if both were asked) is stopped, and the tuning is put back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/schedule.hpp"
#include "engine/engine.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {
namespace sched = cascade::core::sched;
using nlohmann::json;

namespace {

FoxCommand mk(std::uint32_t op, std::uint32_t radio) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.target = radio;
    return c;
}

const char* repeatName(sched::Repeat r) { return r == sched::Repeat::Daily ? "daily" : r == sched::Repeat::Weekly ? "weekly" : "once"; }

std::string describeRadio(int radio) { return "radio " + std::to_string(radio); }

}  // namespace

std::int64_t Engine::scheduleNowMs() const {
    const std::int64_t over = scheduleClockMs_.load(std::memory_order_relaxed);
    if (over != 0) { return over; }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Engine::loadSchedule(const std::vector<sched::Entry>& entries) {
    schedule_.setEntries(sched::sanitise(entries));
    scheduleRt_.clear();
    std::uint32_t top = 0;
    for (const sched::Entry& e : schedule_.entries()) {
        top = std::max(top, e.id);
        ScheduleRt rt;
        rt.state = e.enabled ? (e.lastOutcome == sched::Outcome::Missed ? "missed" : e.lastOutcome == sched::Outcome::Refused ? "refused" : "waiting")
                             : "disabled";
        scheduleRt_[e.id] = std::move(rt);
    }
    scheduleNextId_ = std::max(scheduleNextId_, top + 1);
    ++scheduleVersion_;
}

std::vector<sched::Entry> Engine::scheduleEntries() const { return schedule_.entries(); }

// A plugin's hint, picked by the user, made into an ordinary entry (FOXAPP_OP_SCHEDULE_FROM_HINT). The plugin never makes an entry itself: it only says what it expects.
std::int32_t Engine::scheduleFromHint(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    const long long n = c.ival[0];
    if (n < 1 || n > static_cast<long long>(pluginHints_.size())) {
        why = "there is no plugin hint with that number";
        return FOXAPI_NOT_FOUND;
    }
    const CascadeScheduleHint& h = pluginHints_[static_cast<std::size_t>(n - 1)].hint;
    const int radio = c.ival[1] == 0 ? 1 : static_cast<int>(c.ival[1]);
    json j;
    j["name"] = text.empty() ? std::string(h.label) : text.substr(0, static_cast<std::size_t>(sched::kMaxNameChars));
    j["radio"] = radio;
    j["startUnix"] = h.aosUnixMs / 1000 - 60;    // a minute before it starts
    j["lengthS"] = static_cast<int>(std::min<std::int64_t>(sched::kMaxLengthS, (h.losUnixMs - h.aosUnixMs + 999) / 1000 + 120));
    j["repeat"] = "once";
    j["tune"] = true;
    j["freqHz"] = h.freqHz;
    j["mode"] = static_cast<int>(h.demodMode == 8u ? 0u : h.demodMode);
    j["bandwidthHz"] = h.bandwidthHz;
    j["recordAudio"] = false;
    j["recordIq"] = true;
    j["returnToWhereIWas"] = true;
    FoxCommand set = c;
    set.op = FOXAPP_OP_SCHEDULE_SET;
    set.ival[0] = 0;
    set.ival[1] = 0;
    return applyScheduleOp(set, j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), res, why);
}

std::int32_t Engine::applyScheduleOp(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    const std::int64_t nowMs = scheduleNowMs();
    const std::int64_t nowS = nowMs / 1000;
    std::vector<sched::Entry> list = schedule_.entries();
    const auto findIn = [&list](std::uint32_t id) -> sched::Entry* {
        for (sched::Entry& e : list) {
            if (e.id == id) { return &e; }
        }
        return nullptr;
    };
    const std::uint32_t id = (c.ival[0] > 0 && c.ival[0] < 0x7FFFFFFF) ? static_cast<std::uint32_t>(c.ival[0]) : 0u;
    switch (c.op) {
        case FOXAPP_OP_SCHEDULE_FROM_HINT: return scheduleFromHint(c, text, res, why);
        case FOXAPP_OP_SCHEDULE_SET: {
            sched::Entry e;
            const bool replace = id != 0;
            if (replace) {
                const sched::Entry* old = findIn(id);
                if (old == nullptr) { why = "there is no timed recording with that number"; return FOXAPI_NOT_FOUND; }
                e = *old;
            } else if (static_cast<int>(list.size()) >= sched::kMaxEntries) {
                why = "the schedule is full: it holds " + std::to_string(sched::kMaxEntries) + " entries";
                return FOXAPI_LIMIT;
            }
            const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) { why = "the entry must be a JSON object"; return FOXAPI_BAD_ARGUMENT; }
            static const char* const known[] = {"name", "radio", "startUnix", "lengthS", "repeat", "tune", "freqHz", "mode", "bandwidthHz",
                                                "recordAudio", "recordIq", "returnToWhereIWas", "enabled"};
            for (auto it = j.begin(); it != j.end(); ++it) {
                bool ok = false;
                for (const char* k : known) { ok = ok || it.key() == k; }
                if (!ok) { why = "unknown key '" + it.key().substr(0, 40) + "' in the entry"; return FOXAPI_BAD_ARGUMENT; }
            }
            const auto num = [&j](const char* k, auto& dst) -> bool {
                const auto it = j.find(k);
                if (it == j.end()) { return true; }
                if (!it->is_number()) { return false; }
                dst = static_cast<std::remove_reference_t<decltype(dst)>>(it->get<double>());
                return true;
            };
            const auto flag = [&j](const char* k, bool& dst) -> bool {
                const auto it = j.find(k);
                if (it == j.end()) { return true; }
                if (!it->is_boolean()) { return false; }
                dst = it->get<bool>();
                return true;
            };
            bool good = true;
            if (const auto it = j.find("name"); it != j.end()) {
                if (it->is_string()) { e.name = it->get<std::string>(); } else { good = false; }
            }
            if (const auto it = j.find("startUnix"); it != j.end()) {
                if (it->is_number_integer()) { e.startUnix = it->get<std::int64_t>(); } else { good = false; }
            }
            good = good && num("radio", e.radio) && num("lengthS", e.lengthS) && num("freqHz", e.freqHz) && num("mode", e.mode) &&
                   num("bandwidthHz", e.bandwidthHz) && flag("tune", e.tune) && flag("recordAudio", e.recordAudio) && flag("recordIq", e.recordIq) &&
                   flag("returnToWhereIWas", e.returnToWhereIWas) && flag("enabled", e.enabled);
            if (const auto it = j.find("repeat"); it != j.end()) {
                if (it->is_string()) {
                    const std::string r = it->get<std::string>();
                    if (r == "once") { e.repeat = sched::Repeat::Once; }
                    else if (r == "daily") { e.repeat = sched::Repeat::Daily; }
                    else if (r == "weekly") { e.repeat = sched::Repeat::Weekly; }
                    else { good = false; }
                } else {
                    good = false;
                }
            }
            if (!good) { why = "a field of the entry has the wrong kind of value"; return FOXAPI_BAD_ARGUMENT; }
            if (!replace) {
                // a new entry that does not say what to record records the audio, as the type's default does
                e.id = 0;
            }
            const std::string problem = sched::validate(e, nowS);
            if (!problem.empty()) { why = problem; return FOXAPI_OUT_OF_RANGE; }
            for (const sched::Entry& other : list) {
                if (other.id == e.id && replace) { continue; }
                if (other.enabled && e.enabled && sched::overlaps(e, other, nowS)) {
                    why = "it overlaps the entry '" + other.name + "' on " + describeRadio(e.radio) + ": two recordings cannot share a radio at one time";
                    return FOXAPI_BUSY;
                }
            }
            // Nothing earlier than the grace is waiting to be "missed" for an entry made now or changed now.
            e.lastStartUnix = std::max<std::int64_t>(0, sched::latestStartAtOrBefore(e, nowS - sched::kGraceS - 1));
            e.missed = 0;
            e.lastOutcome = sched::Outcome::None;
            if (replace) {
                *findIn(id) = e;
            } else {
                e.id = scheduleNextId_++;
                e.createdUnix = nowS;
                list.push_back(e);
            }
            res.applied[0] = static_cast<double>(e.id);
            schedule_.setEntries(list);
            ScheduleRt& rt = scheduleRt_[e.id];
            if (!schedule_.active(e.id)) {
                rt = ScheduleRt{};
                rt.state = e.enabled ? "waiting" : "disabled";
            }
            ++scheduleVersion_;
            return FOXAPI_OK;
        }
        case FOXAPP_OP_SCHEDULE_REMOVE: {
            const auto it = std::find_if(list.begin(), list.end(), [id](const sched::Entry& e) { return e.id == id; });
            if (id == 0 || it == list.end()) { why = "there is no timed recording with that number"; return FOXAPI_NOT_FOUND; }
            list.erase(it);
            schedule_.setEntries(list);      // a running recording of it ends at the next pass
            if (!schedule_.active(id)) { scheduleRt_.erase(id); }
            ++scheduleVersion_;
            return FOXAPI_OK;
        }
        case FOXAPP_OP_SCHEDULE_ENABLE: {
            sched::Entry* e = findIn(id);
            if (e == nullptr) { why = "there is no timed recording with that number"; return FOXAPI_NOT_FOUND; }
            const bool on = c.ival[1] != 0;
            if (e->enabled == on) { return FOXAPI_NO_CHANGE; }
            if (on) {
                for (const sched::Entry& other : list) {
                    if (other.id != e->id && other.enabled && sched::overlaps(*e, other, nowS)) {
                        why = "it overlaps the entry '" + other.name + "' on " + describeRadio(e->radio);
                        return FOXAPI_BUSY;
                    }
                }
                e->lastStartUnix = std::max<std::int64_t>(0, sched::latestStartAtOrBefore(*e, nowS - sched::kGraceS - 1));
            }
            e->enabled = on;
            schedule_.setEntries(list);
            ScheduleRt& rt = scheduleRt_[id];
            if (!schedule_.active(id)) { rt.state = on ? "waiting" : "disabled"; rt.note.clear(); }
            ++scheduleVersion_;
            return FOXAPI_OK;
        }
        case FOXAPP_OP_SCHEDULE_RUN_NOW: {
            if (findIn(id) == nullptr) { why = "there is no timed recording with that number"; return FOXAPI_NOT_FOUND; }
            if (!schedule_.runNow(id)) { why = "it is recording now"; return FOXAPI_BUSY; }
            return FOXAPI_OK;
        }
        case FOXAPP_OP_SCHEDULE_SKIP: {
            if (findIn(id) == nullptr) { why = "there is no timed recording with that number"; return FOXAPI_NOT_FOUND; }
            if (!schedule_.skipNext(id, nowS)) { why = "it has no next start to skip"; return FOXAPI_NO_CHANGE; }
            scheduleRt_[id].state = "skipped";
            ++scheduleVersion_;
            return FOXAPI_OK;
        }
        default: break;
    }
    why = "not a schedule op";
    return FOXAPI_UNSUPPORTED;
}

// The reason an entry cannot begin on its radio now, "" when it can. Runs under the radio's RadioWork.
std::string Engine::scheduleRefusal(const sched::Entry& e, std::size_t slot) {
    const std::string who = describeRadio(e.radio);
    if (!receiverLive(static_cast<std::uint32_t>(e.radio))) { return who + " is not open"; }
    RadioWork w(*this, slot);
    cascade::core::PublishedState p;
    if (!rx().receiverSnapshot_->read(p)) { return who + " could not be read just now"; }
    if (rx().deviceOpenPending_) { return who + " is still opening its device"; }
    if ((p.rx.flags & FOXAPI_RX_RUNNING) == 0u) { return who + " is stopped (a timed recording does not start a radio)"; }
    if ((p.rx.flags & FOXAPI_RX_FAULTED) != 0u) { return who + " has a fault"; }
    if (scanner_.active() && scannerSlot_ == slot) { return "the scanner is using " + who; }
    if (e.recordIq && (rx().iqRecorder_.recording() || rx().iqStart_.pending())) { return who + " is already recording I/Q"; }
    if (e.recordAudio && (rx().audioRecorder_.recording() || rx().audioStart_.pending())) { return who + " is already recording audio"; }
    for (const auto& [otherId, rt] : scheduleRt_) {
        if (otherId != e.id && (rt.state == "recording" || rt.state == "starting") && rt.radio == e.radio) {
            return "another timed recording is using " + who;
        }
    }
    return std::string();
}

void Engine::scheduleStopRun(std::uint32_t id, const std::string& why, bool early) {
    ScheduleRt& rt = scheduleRt_[id];
    const std::uint32_t radio = static_cast<std::uint32_t>(rt.radio);
    if (receiverLive(radio)) {
        // What this entry started, it ends - never a recording somebody else began.
        if (rt.audio) {
            FoxCommand c = mk(FOXAPI_OP_RECORD_AUDIO, radio);
            c.ival[0] = 0;
            (void)applyCommand(c);
        }
        if (rt.iq) {
            FoxCommand c = mk(FOXAPI_OP_RECORD_IQ, radio);
            c.ival[0] = 0;
            (void)applyCommand(c);
        }
        if (rt.restore && rt.tuned) {
            FoxCommand a = mk(FOXAPI_OP_SET_CENTRE, radio);
            a.num[0] = rt.prevCentre;
            (void)applyCommand(a);
            FoxCommand b = mk(FOXAPI_OP_SET_VFO_OFFSET, radio);
            b.num[0] = rt.prevOffset;
            (void)applyCommand(b);
            if (rt.modeChanged) {
                FoxCommand m = mk(FOXAPI_OP_SET_MODE, radio);
                m.ival[0] = rt.prevDemod;
                (void)applyCommand(m);
            }
            if (rt.bandwidthChanged) {
                FoxCommand w = mk(FOXAPI_OP_SET_BANDWIDTH, radio);
                w.num[0] = rt.prevBandwidth;
                (void)applyCommand(w);
            }
        }
    }
    schedule_.ended(id);
    rt.state = early ? "stopped early" : "done";
    rt.note = why;
    rt.audio = rt.iq = rt.tuned = false;
    ++scheduleVersion_;
}

void Engine::scheduleBegin(std::uint32_t id, std::int64_t occurrence, std::int64_t nowMs) {
    const sched::Entry* ep = nullptr;
    for (const sched::Entry& x : schedule_.entries()) {
        if (x.id == id) { ep = &x; }
    }
    if (ep == nullptr) { return; }
    const sched::Entry e = *ep;
    ScheduleRt& rt = scheduleRt_[id];
    rt = ScheduleRt{};
    rt.radio = e.radio;
    const std::size_t slot = static_cast<std::size_t>(e.radio) - 1u;
    const std::string refusal = scheduleRefusal(e, slot);
    if (!refusal.empty()) {
        schedule_.refused(id, occurrence);
        rt.state = "refused";
        rt.note = refusal;
        ++scheduleVersion_;
        cascade::core::diagWarnf("schedule: timed recording %u was not started (%s)", static_cast<unsigned>(id), refusal.c_str());
        return;
    }
    const std::uint32_t radio = static_cast<std::uint32_t>(e.radio);
    // Where the radio was, for the way back.
    {
        RadioWork w(*this, slot);
        cascade::core::PublishedState p;
        if (rx().receiverSnapshot_->read(p)) {
            rt.prevCentre = p.rx.centreHz;
            rt.prevOffset = p.rx.vfoOffsetHz;
            rt.prevDemod = p.rx.demodMode;
            rt.prevBandwidth = p.rx.bandwidthHz;
        }
    }
    rt.restore = e.returnToWhereIWas;
    const auto fail = [&](const std::string& why) {
        rt.audio = rt.audio || false;
        scheduleStopRun(id, why, false);   // undoes what was done (it only ends what rt says was started)
        schedule_.refused(id, occurrence);
        rt.state = "refused";
        rt.note = why;
        ++scheduleVersion_;
    };
    // Registered with the clock first so that the undo above has a run to end.
    schedule_.started(id, occurrence, nowMs);
    if (e.tune) {
        FoxCommand f = mk(FOXAPI_OP_SET_FREQUENCY, radio);
        f.num[0] = e.freqHz;
        const FoxCommandResult fr = applyCommand(f);
        if (fr.status < 0) { fail("could not tune " + describeRadio(e.radio) + ": " + fr.message); return; }
        rt.tuned = true;
        if (e.mode != 0) {
            FoxCommand m = mk(FOXAPI_OP_SET_MODE, radio);
            m.ival[0] = e.mode;
            const FoxCommandResult mr = applyCommand(m);
            if (mr.status < 0) { fail(std::string("could not set the mode: ") + mr.message); return; }
            rt.modeChanged = true;
        }
        if (e.bandwidthHz > 0.0) {
            FoxCommand b = mk(FOXAPI_OP_SET_BANDWIDTH, radio);
            b.num[0] = e.bandwidthHz;
            const FoxCommandResult br = applyCommand(b);
            if (br.status < 0) { fail(std::string("could not set the bandwidth: ") + br.message); return; }
            rt.bandwidthChanged = true;
        }
    }
    if (e.recordAudio) {
        FoxCommand r = mk(FOXAPI_OP_RECORD_AUDIO, radio);
        r.ival[0] = 1;
        const FoxCommandResult rr = applyCommand(r);
        if (rr.status < 0) { fail(std::string("the audio recording could not start: ") + rr.message); return; }
        rt.audio = true;
    }
    if (e.recordIq) {
        FoxCommand r = mk(FOXAPI_OP_RECORD_IQ, radio);
        r.ival[0] = 1;
        const FoxCommandResult rr = applyCommand(r);
        if (rr.status < 0) { fail(std::string("the I/Q recording could not start: ") + rr.message); return; }
        rt.iq = true;
    }
    rt.state = "starting";
    rt.startedUnixMs = nowMs;
    ++scheduleVersion_;
    cascade::core::diagLogf("schedule: timed recording %u started on radio %d", static_cast<unsigned>(id), e.radio);
}

void Engine::pumpSchedule() {
    if (schedule_.entries().empty() && schedule_.activeCount() == 0) { return; }
    const std::int64_t nowMs = scheduleNowMs();
    for (const sched::Decision& d : schedule_.tick(nowMs)) {
        switch (d.kind) {
            case sched::Decision::Kind::Start:
                scheduleBegin(d.id, d.occurrence, nowMs);
                break;
            case sched::Decision::Kind::Stop:
                scheduleStopRun(d.id, d.why, d.why.find("length") == std::string::npos);
                break;
            case sched::Decision::Kind::Missed: {
                ScheduleRt& rt = scheduleRt_[d.id];
                if (rt.state != "recording" && rt.state != "starting") {
                    rt.state = "missed";
                    rt.note = std::to_string(d.count) + (d.count == 1 ? " start" : " starts") + " fell while FoxSDR was not running or the clock jumped; none was started late";
                }
                ++scheduleVersion_;
                cascade::core::diagWarnf("schedule: timed recording %u missed %d start(s)", static_cast<unsigned>(d.id), d.count);
                break;
            }
        }
    }
    // The recordings in force: is each doing what it was told?
    for (auto& [id, rt] : scheduleRt_) {
        if (rt.state != "starting" && rt.state != "recording") { continue; }
        const std::uint32_t radio = static_cast<std::uint32_t>(rt.radio);
        if (!receiverLive(radio)) {
            scheduleStopRun(id, describeRadio(rt.radio) + " was closed", true);
            continue;
        }
        const std::size_t slot = static_cast<std::size_t>(rt.radio) - 1u;
        RadioWork w(*this, slot);
        const bool audioOk = !rt.audio || rx().audioRecorder_.recording();
        const bool iqOk = !rt.iq || rx().iqRecorder_.recording();
        const bool pending = (rt.iq && rx().iqStart_.pending()) || (rt.audio && rx().audioStart_.pending());
        if (rt.state == "starting") {
            ++rt.passes;
            if (audioOk && iqOk) {
                rt.state = "recording";
                rt.note.clear();
                ++scheduleVersion_;
            } else if (!pending && rt.passes > 3) {
                // The open ended with nothing recording: the recorder's own sentence is the reason (a folder that cannot be written, a full disk).
                std::string why = rx().recordError_;
                if (why.empty()) { why = "the recording did not open"; }
                scheduleStopRun(id, why, false);
                schedule_.refused(id, 0);
                rt.state = "refused";
                rt.note = why;
                ++scheduleVersion_;
                cascade::core::diagWarnf("schedule: timed recording %u refused: %s", static_cast<unsigned>(id), why.c_str());
            }
        } else if (!audioOk || !iqOk) {
            // It was recording and is not: the disk filled, the source changed, the recorder gave up. The files that were written are closed and whole.
            std::string why = !rx().recordError_.empty() ? rx().recordError_ : !rx().recordNotice_.empty() ? rx().recordNotice_ : "the recording ended on its own";
            scheduleStopRun(id, why, true);
        }
    }
    if (schedule_.dirty()) {
        schedule_.clearDirty();
        ++scheduleVersion_;
    }
}

void Engine::scheduleFacts(const std::function<void(const std::string&, const std::string&)>& put) const {
    const std::vector<sched::Entry>& list = schedule_.entries();
    std::int64_t nextS = 0;
    int missed = 0;
    const std::int64_t nowS = scheduleNowMs() / 1000;
    for (const sched::Entry& e : list) {
        missed += e.missed;
        if (!e.enabled) { continue; }
        const std::int64_t n = sched::nextStartAfter(e, std::max(nowS, e.lastStartUnix));
        if (n > 0 && (nextS == 0 || n < nextS)) { nextS = n; }
    }
    put("schedule.count", std::to_string(list.size()));
    put("schedule.active", std::to_string(schedule_.activeCount()));
    put("schedule.missed", std::to_string(missed));
    put("schedule.max", std::to_string(sched::kMaxEntries));
    put("schedule.next", std::to_string(nextS));
    // WHAT PLUGINS PREDICT, for the user to pick (FOXAPP_OP_SCHEDULE_FROM_HINT): one JSON document per hint, numbered from 1 in the order the plugins gave them.
    put("schedule.hint.count", std::to_string(pluginHints_.size()));
    for (std::size_t i = 0; i < pluginHints_.size(); ++i) {
        const CascadeScheduleHint& h = pluginHints_[i].hint;
        json hj;
        hj["plugin"] = pluginHints_[i].plugin;
        hj["id"] = h.id;
        hj["kind"] = h.kind == CASCADE_SCHEDULE_HINT_PASS ? "pass" : "other";
        hj["label"] = std::string(h.label);
        hj["aosUnix"] = h.aosUnixMs / 1000;
        hj["losUnix"] = h.losUnixMs / 1000;
        hj["maxElDeg"] = h.maxElDeg;
        hj["freqHz"] = h.freqHz;
        hj["mode"] = h.demodMode;
        hj["bandwidthHz"] = h.bandwidthHz;
        hj["decoder"] = std::string(h.decoderName);
        put("schedule.hint." + std::to_string(i + 1), hj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
    for (const sched::Entry& e : list) {
        json j;
        j["id"] = e.id;
        j["name"] = e.name;
        j["radio"] = e.radio;
        j["startUnix"] = e.startUnix;
        j["lengthS"] = e.lengthS;
        j["repeat"] = repeatName(e.repeat);
        j["tune"] = e.tune;
        j["freqHz"] = e.freqHz;
        j["mode"] = e.mode;
        j["bandwidthHz"] = e.bandwidthHz;
        j["recordAudio"] = e.recordAudio;
        j["recordIq"] = e.recordIq;
        j["returnToWhereIWas"] = e.returnToWhereIWas;
        j["enabled"] = e.enabled;
        j["lastStartUnix"] = e.lastStartUnix;
        j["missed"] = e.missed;
        const auto it = scheduleRt_.find(e.id);
        j["state"] = it != scheduleRt_.end() ? it->second.state : std::string("waiting");
        j["note"] = it != scheduleRt_.end() ? it->second.note : std::string();
        j["nextUnix"] = e.enabled ? sched::nextStartAfter(e, std::max(nowS, e.lastStartUnix)) : -1;
        put("schedule.entry." + std::to_string(e.id), j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
}

}  // namespace cascade::engine
