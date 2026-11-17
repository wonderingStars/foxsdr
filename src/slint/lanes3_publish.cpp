// lanes3_publish.cpp - see lanes3_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanes3_publish.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "core/overview_job.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "lanes3_logic.hpp"
#include "plugin_kit.hpp"

namespace foxsdr::slintlanes3 {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
using nlohmann::json;
using slk::Row;
using slk::kDim;
using slk::kErr;
using slk::kGood;
using slk::kWarn;
using slk::rChoice;
using slk::rField;
using slk::rHeading;
using slk::rKey;
using slk::rNote;
using slk::rReadout;
using slk::rSlider;
using slk::rTick;
using sll::numberText;
using sll::trim;

// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

KRow toK(const Row& r) {
    KRow k;
    k.kind = r.kind;
    k.id = ss(r.id);
    k.label = ss(r.label);
    k.value = ss(r.value);
    k.note = ss(r.note);
    k.on = r.on;
    k.enabled = r.enabled;
    std::vector<slint::SharedString> opts;
    for (const auto& o : r.options) { opts.push_back(ss(o)); }
    k.options = std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(opts));
    k.pick = r.pick;
    k.level = r.level;
    k.tone = r.tone;
    return k;
}

enum Sec { kRecord = 0, kSchedule, kSource, kPlay, kSecCount };
const char* const kSecName[kSecCount] = {"record", "schedule", "source", "play"};

struct Cmd {
    std::uint32_t op = 0;
    long long i0 = 0, i1 = 0;
    double n0 = 0, n1 = 0;
    std::string text;
};
Cmd cmd(std::uint32_t op, long long i0 = 0, const std::string& text = std::string()) { Cmd c; c.op = op; c.i0 = i0; c.text = text; return c; }
}  // namespace

struct LaneS3Publisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    std::map<std::string, std::string> draft;     // what the person typed or chose and has not sent (never saved)
    struct Pending { int sec = 0; std::string what; };
    std::map<std::uint64_t, Pending> pending;     // ticket -> section, what it was
    std::map<std::uint64_t, std::vector<std::string>> pendingDraft;   // ticket -> the fields whose typed text is dropped once the engine has taken it
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> sig;
    std::array<std::string, kSecCount> lastCount;
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    long lastRebuildTick = -100;
    long lastPlayTick = -100;
    std::string armed;                            // the id of a destructive key pressed once ("press again")

    cascade::core::overview::Job ovJob;       // G3-14b: the whole-file waterfall's job
    std::string ovState = "idle";              // idle, running, done, failed, cancelled
    int ovW = 0, ovH = 0;
    std::string ovCaption;
    bool ovVisible = false;
    slint::Image ovImage;

    void pollOverview() {
        cascade::core::overview::Result r;
        if (!ovJob.take(r)) { return; }
        if (r.ok) {
            slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(static_cast<std::uint32_t>(r.image.width), static_cast<std::uint32_t>(r.image.height));
            std::memcpy(reinterpret_cast<std::uint8_t*>(buf.begin()), r.image.rgba.data(), r.image.rgba.size());
            ovImage = slint::Image(buf);
            ovW = r.image.width; ovH = r.image.height;
            ovCaption = tr("Time runs down, frequency across") + "  " + numberText(std::round(r.image.rateHz / 100.0) / 10.0) + " kS/s  " + numberText(std::round(r.image.durationS)) + " s  " + std::to_string(r.image.height) + " x " + std::to_string(r.image.width) + "  " + numberText(std::round(r.elapsedS * 10.0) / 10.0) + " s";
            ovVisible = true;
            ovState = "done";
            setNote(kSource, tr("Whole-file waterfall") + ": " + tr("done"), kGood);
            if (env.ui != nullptr) {
                auto& l = env.ui->global<LaneS3>();
                l.set_overview(ovImage);
                l.set_overview_caption(ss(ovCaption));
                l.set_overview_visible(true);
            }
        } else {
            ovState = r.cancelled ? "cancelled" : "failed";
            setNote(kSource, tr("Whole-file waterfall") + ": " + (r.cancelled ? tr("cancelled") : r.error), r.cancelled ? kDim : kErr);
            if (!r.cancelled) { ++refused; }
        }
        dirty = true;
    }

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {}

    // ---------------------------------------------------------------- facts, state and drafts
    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    int sel() { const int s = env.selectedRadio ? env.selectedRadio() : 1; return s < 1 ? 1 : s; }
    std::string rn() { return std::to_string(sel()); }
    PlayState ps() const { return env.playState ? env.playState() : PlayState{}; }
    std::int64_t now() const { return env.nowUnix ? env.nowUnix() : 0; }
    OffsetAt offs() const { return env.offsetAt ? OffsetAt(env.offsetAt) : OffsetAt([](std::int64_t) { return 0; }); }
    std::string dv(const std::string& id, const std::string& def = std::string()) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second; }
    int pv(const std::string& id, int def) const { const auto it = draft.find(id); return it == draft.end() ? def : std::atoi(it->second.c_str()); }
    static double numOf(const std::string& s, double def = 0.0) { return s.empty() ? def : std::atof(s.c_str()); }
    bool bv(const std::string& id, bool def) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second == "1"; }

    // ---------------------------------------------------------------- the one place a command leaves
    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    void send(Sec s, const std::string& what, const Cmd& k, const std::vector<std::string>& draftsToDrop = {}) {
        if (!env.submit) { return; }
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = k.op;
        c.ival[0] = k.i0;
        c.ival[1] = k.i1;
        c.num[0] = k.n0; c.num[1] = k.n1;
        const std::string* longp = nullptr;
        if (k.text.size() < sizeof(c.text)) { std::memcpy(c.text, k.text.data(), k.text.size()); } else { longp = &k.text; }
        const SubmitOutcome o = env.submit(c, 0, longp);
        ++sent;
        if (o.ticket == 0 || sll::statusIsRefusal(o.status)) {
            ++refused;
            setNote(s, what + ": " + sll::refusalText(o.status, o.message), kErr);
        } else {
            pending[o.ticket] = {static_cast<int>(s), what};
            if (!draftsToDrop.empty()) { pendingDraft[o.ticket] = draftsToDrop; }
            setNote(s, what + ": " + tr("sent"), kDim);
        }
    }
    void refuseHere(Sec s, const std::string& what, const std::string& why) { ++refused; setNote(s, what + ": " + why, kErr); }
    void sendBuilt(Sec s, const std::string& what, const sll::Built& b, std::uint32_t op, long long i0 = 0, const std::vector<std::string>& drops = {}) {
        if (!b.ok) { refuseHere(s, what, b.err); return; }
        send(s, what, cmd(op, i0, b.text), drops);
    }

    void onAnswer(const FoxCommandResult& r) {
        const auto it = pending.find(r.ticket);
        if (it == pending.end()) { return; }
        const Sec s = static_cast<Sec>(it->second.sec);
        const std::string what = it->second.what;
        pending.erase(it);
        ++answered;
        const auto pd = pendingDraft.find(r.ticket);
        if (pd != pendingDraft.end()) {
            if (r.status == FOXAPI_OK || r.status == FOXAPI_NO_CHANGE) { for (const auto& d : pd->second) { draft.erase(d); } }
            pendingDraft.erase(pd);
        }
        const std::string msg = trim(r.message);
        if (r.status == FOXAPI_OK) { setNote(s, what + ": " + tr("done") + (msg.empty() ? std::string() : " (" + msg + ")"), kGood); }
        else if (r.status == FOXAPI_NO_CHANGE) { setNote(s, what + ": " + sll::refusalText(r.status, msg), kDim); }
        else { ++refused; setNote(s, what + ": " + sll::refusalText(r.status, msg), kErr); }
        dirty = true;
    }

    // ---------------------------------------------------------------- row helpers (each registers the row's handler)
    void field(std::vector<Row>& v, const std::string& id, const std::string& label, const std::string& def, const std::string& tip = std::string()) {
        v.push_back(rField(id, label, dv(id, def), tip));
        handlers[id] = [this, id](const std::string& t) { draft[id] = t; dirty = true; };
    }
    void key(std::vector<Row>& v, const std::string& id, const std::string& label, std::function<void()> fn, bool enabled = true, const std::string& tip = std::string()) {
        v.push_back(rKey(id, label, enabled, tip));
        handlers[id] = [fn](const std::string&) { fn(); };
    }
    void tick(std::vector<Row>& v, const std::string& id, const std::string& label, bool on, std::function<void(bool)> fn, const std::string& tip = std::string()) {
        v.push_back(rTick(id, label, on, true, tip));
        handlers[id] = [this, fn](const std::string& t) { fn(t == "1"); dirty = true; };
    }
    void choice(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int pick, std::function<void(int)> fn, const std::string& tip = std::string()) {
        v.push_back(rChoice(id, label, opts, pick, true, tip));
        handlers[id] = [this, fn](const std::string& t) { fn(std::atoi(t.c_str())); dirty = true; };
    }
    void choiceD(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int def) {
        choice(v, id, label, opts, pv(id, def), [this, id](int i) { draft[id] = std::to_string(i); });
    }
    void tickD(std::vector<Row>& v, const std::string& id, const std::string& label, bool def, const std::string& tip = std::string()) {
        tick(v, id, label, bv(id, def), [this, id](bool on) { draft[id] = on ? "1" : "0"; }, tip);
    }
    void setting(Sec s, const std::string& what, const std::string& k, const std::string& value, const std::vector<std::string>& drops = {}) {
        send(s, what, cmd(FOXAPP_OP_WIREC_SETTING, 0, k + "" + value), drops);
    }
    void endNote(std::vector<Row>& v, Sec s) { if (!noteText[s].empty()) { v.push_back(rNote(noteText[s], noteTone[s])); } }
    bool confirmTwice(Sec s, const std::string& id, const std::string& what) {
        if (armed != id) { armed = id; setNote(s, what + ": " + tr("press the key again to confirm. This cannot be undone."), kWarn); return false; }
        armed.clear();
        return true;
    }

    // ================================================================ SIGNAL > Recorder
    void buildRecord(std::vector<Row>& v) {
        // ---- file names (G3-08)
        const std::string inForce = fact("record.nameTemplate");
        v.push_back(rHeading(tr("File names")));
        v.push_back(rReadout(tr("In force"), inForce.empty() ? tr("the default names") : inForce));
        field(v, "n.template", tr("Name template"), inForce, nameTokensHelp());
        const std::string typed = dv("n.template", inForce);
        const NameCheck chk = checkNameTemplate(typed);
        if (chk.ok) { v.push_back(rReadout(tr("A take would be called"), chk.example)); }
        else { v.push_back(rNote(chk.error, kErr)); }
        v.push_back(rNote(nameTokensHelp()));
        key(v, "n.apply", tr("Use this template"), [this] {
            const std::string t = trim(dv("n.template", fact("record.nameTemplate")));
            const NameCheck c = checkNameTemplate(t);
            if (!c.ok) { refuseHere(kRecord, tr("Use this template"), c.error); return; }
            send(kRecord, tr("Use this template"), cmd(FOXAPP_OP_SET_RECORD_NAME, 0, t), {"n.template"});
        }, chk.ok && !trim(typed).empty(), tr("Every recording from now on is named this way. A name that exists is never written over."));
        key(v, "n.default", tr("Back to the default names"), [this] { send(kRecord, tr("Back to the default names"), cmd(FOXAPP_OP_SET_RECORD_NAME, 0, ""), {"n.template"}); }, !inForce.empty());

        // ---- sample formats (G3-07)
        const PlayState p = ps();
        int iq = p.recIq, audio = p.recAudio;
        { int a = 0, b = 0; if (parseFormatFact(fact("radio." + rn() + ".record.format"), a, b)) { iq = a; audio = b; } }
        v.push_back(rHeading(tr("Sample format")));
        choice(v, "f.iq", tr("I/Q takes"), iqFormatNames(), iq, [this, audio](int i) { Cmd c = cmd(FOXAPI_OP_SET_RECORD_FORMAT, i); c.i1 = audio; send(kRecord, tr("I/Q takes"), c); },
               tr("32-bit float keeps every sample exactly. 16-bit integer is half the size; a sample beyond full scale is clipped."));
        const double rate = p.sampleRateHz > 0.0 ? p.sampleRateHz : 2.0e6;
        v.push_back(rReadout(tr("An I/Q take costs"), costPerSecond(true, iq, rate)));
        choice(v, "f.audio", tr("Audio takes"), audioFormatNames(), audio, [this, iq](int i) { Cmd c = cmd(FOXAPI_OP_SET_RECORD_FORMAT, iq); c.i1 = i; send(kRecord, tr("Audio takes"), c); },
               tr("For takes that start after this; one under way keeps its format."));
        v.push_back(rReadout(tr("An audio take costs"), costPerSecond(false, audio, 48000.0)));

        // ---- the passband take (G3-04)
        const bool ifOn = fact("radio." + rn() + ".ifrec") == "1";
        v.push_back(rHeading(tr("Passband recording (IF)")));
        v.push_back(rReadout(tr("Passband take"), ifOn ? tr("recording") : tr("not recording")));
        key(v, "i.start", tr("Record the passband"), [this] { send(kRecord, tr("Record the passband"), cmd(FOXAPP_OP_IF_RECORD, 1)); }, !ifOn,
            tr("Records the channel after the tuner and filter, before the demodulator: the same in every mode. It ends with the radio."));
        key(v, "i.stop", tr("Stop the passband take"), [this] { send(kRecord, tr("Stop the passband take"), cmd(FOXAPP_OP_IF_RECORD, 0)); }, ifOn);

        // ---- long recordings (G3-10): the next file, no sample lost
        const std::string mbNow = fact("wirec.rec.roll_mb"), minNow = fact("wirec.rec.roll_min");
        v.push_back(rHeading(tr("Long recordings")));
        { std::string nowText; if (numOf(mbNow) > 0.0) { nowText += numberText(numOf(mbNow)) + " MB"; } if (numOf(minNow) > 0.0) { nowText += (nowText.empty() ? "" : " " + tr("or") + " ") + numberText(numOf(minNow)) + " " + tr("minutes") + (fact("wirec.rec.roll_align") == "1" ? ", " + tr("on the clock (UTC)") : std::string()); }
          v.push_back(rReadout(tr("A recording goes on into a new file after"), nowText.empty() ? tr("no limit") : nowText)); }
        field(v, "o.mb", tr("File size limit (MB, 0 = none)"), mbNow.empty() ? "0" : mbNow, tr("1 to 100000. A file ends at the first block of samples after the limit; the next one starts with the next sample."));
        field(v, "o.min", tr("File length limit (minutes, 0 = none)"), minNow.empty() ? "0" : minNow, tr("0.1 to 1440."));
        tickD(v, "o.align", tr("New file on the clock (UTC)"), fact("wirec.rec.roll_align") == "1", tr("With a length limit: a file ends at every whole multiple of it, for example at :00 and :30 for 30 minutes."));
        key(v, "o.apply", tr("Use these limits"), [this] {
            const sll::Built b = rolloverJson(dv("o.mb", fact("wirec.rec.roll_mb")), dv("o.min", fact("wirec.rec.roll_min")), bv("o.align", fact("wirec.rec.roll_align") == "1"));
            if (!b.ok) { refuseHere(kRecord, tr("Use these limits"), b.err); return; }
            const json j = json::parse(b.text);
            setting(kRecord, tr("Use these limits"), "rec.roll_mb", numberText(j.value("maxMB", 0.0)));
            setting(kRecord, tr("Use these limits"), "rec.roll_min", numberText(std::round(j.value("maxSeconds", 0.0) / 60.0 * 1000.0) / 1000.0));
            setting(kRecord, tr("Use these limits"), "rec.roll_align", j.value("align", false) ? "1" : "0", {"o.mb", "o.min", "o.align"});
        }, true, tr("Applies to the recordings that start after this, and to one that is running."));

        // ---- one file per transmission (G3-03, G3-09) and the replay buffer (G3-02)
        const bool clipOn = fact("wirec.rec.clip_on") == "1";
        v.push_back(rHeading(tr("One file per transmission")));
        tick(v, "c.on", tr("Record while the squelch is open"), clipOn, [this](bool on) { setting(kRecord, tr("Record while the squelch is open"), "rec.clip_on", on ? "1" : "0"); },
             tr("Each transmission is its own file in the clips folder of the recordings folder. Set the squelch first: with it always open there is one long clip after another."));
        { const std::string cs = fact("radio." + rn() + ".record.clips"); if (!cs.empty()) { v.push_back(rReadout(tr("Clips"), cs)); } }
        const auto cf = [this](const char* key, const char* def) { const std::string f = fact(std::string("wirec.rec.") + key); return f.empty() ? std::string(def) : f; };
        field(v, "c.pre", tr("Time before the transmission (s)"), cf("clip_pre", "2"), tr("0 to 10. Needs the replay buffer, which this raises for itself."));
        field(v, "c.tail", tr("Time after it ends (s)"), cf("clip_tail", "3"), tr("0 to 30. A transmission that resumes inside this goes on in the same file."));
        field(v, "c.min", tr("Shortest clip kept (s)"), cf("clip_min", "1"), tr("0 to 60. A shorter transmission is not kept."));
        field(v, "c.max", tr("Longest clip (s)"), cf("clip_max", "600"), tr("10 to 3600. A longer transmission goes on in a new file."));
        key(v, "c.apply", tr("Use these times"), [this, cf] {
            ClipFields f; f.enabled = fact("wirec.rec.clip_on") == "1";
            f.preRollS = dv("c.pre", cf("clip_pre", "2")); f.tailS = dv("c.tail", cf("clip_tail", "3")); f.minS = dv("c.min", cf("clip_min", "1")); f.maxClipS = dv("c.max", cf("clip_max", "600"));
            const sll::Built b = clipsJson(f);
            if (!b.ok) { refuseHere(kRecord, tr("Use these times"), b.err); return; }
            const json j = json::parse(b.text);
            setting(kRecord, tr("Use these times"), "rec.clip_pre", numberText(j.value("preRollS", 2.0)));
            setting(kRecord, tr("Use these times"), "rec.clip_tail", numberText(j.value("tailS", 3.0)));
            setting(kRecord, tr("Use these times"), "rec.clip_min", numberText(j.value("minS", 1.0)));
            setting(kRecord, tr("Use these times"), "rec.clip_max", numberText(j.value("maxClipS", 600.0)), {"c.pre", "c.tail", "c.min", "c.max"});
        });
        const std::string rpNow = cf("replay_s", "0");
        v.push_back(rHeading(tr("Replay buffer")));
        field(v, "p.replay", tr("Audio kept in memory (seconds, 0 = none)"), rpNow, tr("0 to 120. 96 kB a second for each radio. An audio recording that starts begins with this much of what was just heard."));
        v.push_back(rReadout(tr("It costs"), replayCost(numOf(dv("p.replay", rpNow)), 48000.0)));
        { const std::string rp = fact("radio." + rn() + ".record.replay"); if (!rp.empty()) { v.push_back(rReadout(tr("Held now"), rp)); } }
        key(v, "p.apply", tr("Keep this much audio"), [this] {
            double sec = 0; std::string e;
            if (!parseReplaySeconds(dv("p.replay", fact("wirec.rec.replay_s")), 120.0, sec, e)) { refuseHere(kRecord, tr("Keep this much audio"), e); return; }
            setting(kRecord, tr("Keep this much audio"), "rec.replay_s", numberText(sec), {"p.replay"});
        });
        endNote(v, kRecord);
    }

    // ================================================================ SIGNAL > Recorder: timed recordings (G3-01)
    std::vector<int> scheduleIds() {
        std::vector<int> ids;
        std::stringstream ss(fact("record.schedule.ids"));
        std::string tok;
        while (std::getline(ss, tok, ',')) { const int n = std::atoi(tok.c_str()); if (n > 0) { ids.push_back(n); } }
        return ids;
    }

    void buildSchedule(std::vector<Row>& v) {
        const OffsetAt off = offs();
        const int zone = env.offsetAt ? env.offsetAt(now() * 1000) : 0;
        char zb[32];
        std::snprintf(zb, sizeof(zb), "UTC%+03d:%02d", zone / 60, std::abs(zone % 60));
        v.push_back(rHeading(tr("Timed recordings")));
        v.push_back(rReadout(tr("Times are in"), std::string(zb) + " (" + tr("this computer's time") + ")"));
        const std::int64_t next = std::atoll(fact("schedule.next").c_str());
        v.push_back(rReadout(tr("Next start"), next > 0 ? unixToLocal(next, off) : tr("none")));
        const int missed = std::atoi(fact("schedule.missed").c_str());
        if (missed > 0) { v.push_back(rNote(std::to_string(missed) + " " + tr("timed recordings were due while FoxSDR was closed and did not happen. They are never started late."), kWarn)); }
        for (const int id : scheduleIds()) {
            const EntryView e = entryView(fact("schedule.entry." + std::to_string(id)), off);
            if (!e.ok) { continue; }
            const std::string label = e.name.empty() ? std::to_string(id) : e.name;
            v.push_back(rNote(std::to_string(id) + ". " + (e.name.empty() ? std::string() : e.name + ": ") + e.line + (e.note.empty() ? std::string() : " (" + e.note + ")"),
                              e.state == "refused" ? kErr : (e.state == "missed" ? kWarn : (e.state == "recording" ? kGood : kDim))));
            const std::string sid = std::to_string(id);
            key(v, "t.run" + sid, tr("Record now") + " " + sid, [this, id] { send(kSchedule, tr("Record now"), cmd(FOXAPP_OP_SCHEDULE_RUN_NOW, id)); }, e.enabled && e.state != "recording");
            key(v, "t.skip" + sid, tr("Skip the next start of") + " " + sid, [this, id] { send(kSchedule, tr("Skip the next start"), cmd(FOXAPP_OP_SCHEDULE_SKIP, id)); }, e.enabled);
            key(v, "t.onoff" + sid, (e.enabled ? tr("Switch off") : tr("Switch on")) + " " + sid, [this, id, en = e.enabled] { Cmd c = cmd(FOXAPP_OP_SCHEDULE_ENABLE, id); c.i1 = en ? 0 : 1; send(kSchedule, en ? tr("Switch off") : tr("Switch on"), c); });
            key(v, "t.rm" + sid, tr("Remove timed recording") + " " + sid, [this, id, sid] {
                if (!confirmTwice(kSchedule, "t.rm" + sid, tr("Remove timed recording"))) { return; }
                send(kSchedule, tr("Remove timed recording"), cmd(FOXAPP_OP_SCHEDULE_REMOVE, id));
            });
        }
        // ---- the form
        std::string defDate, defTime;
        localDateTimeParts(now() + 300, off, defDate, defTime);
        v.push_back(rHeading(tr("New timed recording")));
        field(v, "t.name", tr("Name"), "", tr("Up to 40 characters; shown in the list."));
        field(v, "t.date", tr("Date (year-month-day)"), defDate, tr("Your computer's date, for example 2026-11-17."));
        field(v, "t.time", tr("Time (24 hours)"), defTime, tr("Your computer's time, for example 14:30."));
        field(v, "t.len", tr("Length (minutes)"), "10", tr("Up to 1440 (a day)."));
        field(v, "t.radio", tr("Radio (1 to 8)"), rn());
        choiceD(v, "t.rep", tr("Repeat"), {tr("Once"), tr("Daily"), tr("Weekly")}, 0);
        tickD(v, "t.tune", tr("Tune the radio first"), false);
        if (bv("t.tune", false)) {
            field(v, "t.freq", tr("Frequency (MHz)"), "");
            choiceD(v, "t.mode", tr("Mode"), {tr("leave as it is"), "NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB"}, 0);
            field(v, "t.bw", tr("Bandwidth (kHz)"), "", tr("Empty leaves the bandwidth as it is."));
            tickD(v, "t.back", tr("Put the tuning back afterwards"), true);
        }
        tickD(v, "t.audio", tr("Record the audio"), true);
        tickD(v, "t.iq", tr("Record the I/Q"), false);
        key(v, "t.add", tr("Add timed recording"), [this] {
            ScheduleFields f;
            f.name = dv("t.name");
            std::string d, t;
            localDateTimeParts(now() + 300, offs(), d, t);
            f.date = trim(dv("t.date")).empty() ? d : dv("t.date");
            f.time = trim(dv("t.time")).empty() ? t : dv("t.time");
            f.lengthMin = dv("t.len", "10");
            long long radio = 1;
            std::string e;
            if (!sll::parseInteger(dv("t.radio", rn()), 1, 8, radio, e)) { refuseHere(kSchedule, tr("Add timed recording"), tr("Radio (1 to 8)") + ": " + e); return; }
            f.radio = static_cast<int>(radio);
            f.repeat = pv("t.rep", 0);
            f.tune = bv("t.tune", false);
            f.freqMHz = dv("t.freq");
            f.mode = pv("t.mode", 0);
            f.bandwidthKHz = dv("t.bw");
            f.returnToWhereIWas = bv("t.back", true);
            f.recordAudio = bv("t.audio", true);
            f.recordIq = bv("t.iq", false);
            sendBuilt(kSchedule, tr("Add timed recording"), scheduleJson(f, offs()), FOXAPP_OP_SCHEDULE_SET, 0, {"t.name"});
        }, true, tr("The time is stored as a UTC instant, so it does not move if the computer's zone changes. No Windows task or wake timer is made: FoxSDR must be running at that time."));
        endNote(v, kSchedule);
    }

    // The position and the seek slider move with every tick, so they are a list of their own: the typed fields below keep their model (and their focus) while a file plays.
    void buildPlay(std::vector<Row>& v) {
        const PlayState p = ps();
        if (!(p.filePlays && p.durationS > 0.0)) { return; }
        v.push_back(rReadout(tr("Now at"), positionText(p.positionS) + " / " + positionText(p.durationS)));
        v.push_back(rSlider("s.seek", tr("Seek"), static_cast<float>(std::clamp(p.positionS / p.durationS, 0.0, 1.0)), positionText(p.positionS), true, tr("Drag to play from another place in the recording.")));
        handlers["s.seek"] = [this](const std::string& t) {
            const double f = std::clamp(std::atof(t.c_str()), 0.0, 1.0);
            Cmd c = cmd(FOXAPP_OP_FILE_SEEK, 1); c.n0 = f;
            send(kSource, tr("Seek"), c);
        };
    }

    // ================================================================ SIGNAL > Source: playback position, loop, raw files (G3-13, G3-14a, G3-15)
    void buildSource(std::vector<Row>& v) {
        const PlayState p = ps();
        if (p.filePlays && p.durationS > 0.0) {
            v.push_back(rHeading(tr("Where in the recording")));
            field(v, "s.goto", tr("Go to (seconds or minutes:seconds)"), "");
            key(v, "s.go", tr("Play from here"), [this] {
                const PlayState q = ps();
                double s = 0.0; std::string e;
                if (!parsePosition(dv("s.goto"), q.durationS, s, e)) { refuseHere(kSource, tr("Play from here"), e); return; }
                Cmd c = cmd(FOXAPP_OP_FILE_SEEK, 0); c.n0 = s;
                send(kSource, tr("Play from here"), c, {"s.goto"});
            });
            const bool loopOn = p.loopEndS > p.loopStartS;
            v.push_back(rReadout(tr("Loop"), loopOn ? positionText(p.loopStartS) + " - " + positionText(p.loopEndS) : tr("none")));
            field(v, "s.la", tr("Loop start"), loopOn ? positionText(p.loopStartS) : "");
            field(v, "s.lb", tr("Loop end"), loopOn ? positionText(p.loopEndS) : "");
            key(v, "s.loop", tr("Loop between these"), [this] {
                const PlayState q = ps();
                double a = 0, b = 0; std::string e;
                if (!parseLoop(dv("s.la"), dv("s.lb"), q.durationS, a, b, e)) { refuseHere(kSource, tr("Loop between these"), e); return; }
                Cmd c = cmd(FOXAPP_OP_FILE_LOOP, 1); c.n0 = a; c.n1 = b;
                send(kSource, tr("Loop between these"), c, {"s.la", "s.lb"});
            });
            key(v, "s.noloop", tr("Clear the loop"), [this] { send(kSource, tr("Clear the loop"), cmd(FOXAPP_OP_FILE_LOOP, 0), {"s.la", "s.lb"}); }, loopOn);
            if (p.startUnixMs > 0.0) { v.push_back(rReadout(tr("Recorded at"), unixToLocal(static_cast<std::int64_t>(p.startUnixMs / 1000.0), offs()))); }
            const std::string hdr = fact("radio." + rn() + ".file.header");
            if (hdr == "1") { v.push_back(rReadout(tr("Centre frequency"), tr("taken from the file's own header"))); }
        }
        // The raw-file and waterfall rows are many: they are folded away (the Source section is open at every start, and the sections below it must stay in reach).
        tickD(v, "src.fold", tr("Raw files and the whole-file waterfall"), false, tr("Opens the rows that play a file with no header and make one picture of a whole recording."));
        if (!bv("src.fold", false)) { endNote(v, kSource); return; }
        // ---- a raw file: it has no header, so the format and the rate are stated here
        v.push_back(rHeading(tr("Open a raw I/Q file")));
        v.push_back(rNote(tr("A raw file holds only samples. Say how they are stored and the rate they were recorded at. A wrong guess plays as noise, never harms anything.")));
        field(v, "r.path", tr("Raw file (full path)"), "", tr("For example C:\\Recordings\\capture.cu8"));
        choiceD(v, "r.fmt", tr("Sample format"), rawFormatNames(), 0);
        field(v, "r.rate", tr("Sample rate (samples a second)"), "2048000", tr("For example 2048000, 2.048M or 250k."));
        field(v, "r.centre", tr("Centre frequency (MHz, optional)"), "", tr("Empty leaves the frequency at 0."));
        key(v, "r.open", tr("Open raw file"), [this] {
            const sll::Built b = rawFileId(dv("r.path"), pv("r.fmt", 0), dv("r.rate", "2048000"), dv("r.centre"));
            if (!b.ok) { refuseHere(kSource, tr("Open raw file"), b.err); return; }
            send(kSource, tr("Open raw file"), cmd(FOXAPI_OP_SELECT_SOURCE, 0, b.text));
        });
        // ---- the whole-file waterfall (G3-14b)
        v.push_back(rHeading(tr("Whole-file waterfall")));
        v.push_back(rNote(tr("One picture of a whole recording, made in the background: time down, frequency across. It takes a sample of each piece of time, so a signal shorter than a few seconds in a long file can be missed. A raw file (.cu8, .cs16, .cf32) uses the sample rate typed above.")));
        field(v, "w.path", tr("Recording (full path)"), "", tr("A WAV, SigMF or raw file. Not the one that is playing: any file."));
        field(v, "w.rows", tr("Rows (50 to 2000)"), "600");
        choiceD(v, "w.fft", tr("Frequency detail (bins)"), overviewFftNames(), 1);
        const bool ovRun = ovJob.running();
        v.push_back(rReadout(tr("Picture"), ovState == "running" ? tr("being made") : ovState == "done" ? tr("ready") : ovState == "failed" ? tr("failed") : ovState == "cancelled" ? tr("cancelled") : tr("none")));
        if (ovRun) { v.push_back(slk::rBar(tr("Progress"), static_cast<float>(ovJob.progress()), std::to_string(static_cast<int>(ovJob.progress() * 100.0)) + " %")); }
        key(v, "w.make", tr("Make the waterfall"), [this] {
            const sll::Built b = overviewJson(dv("w.rows", "600"), pv("w.fft", 1));
            if (!b.ok) { refuseHere(kSource, tr("Make the waterfall"), b.err); return; }
            const json j = json::parse(b.text);
            cascade::core::overview::Request rq;
            rq.path = trim(dv("w.path"));
            if (rq.path.empty()) { refuseHere(kSource, tr("Make the waterfall"), tr("type the full path of the recording")); return; }
            rq.rows = j.value("rows", 600);
            rq.fft = j.value("fft", 1024);
            std::string ext = rq.path.size() >= 5 ? rq.path.substr(rq.path.find_last_of('.') == std::string::npos ? 0 : rq.path.find_last_of('.')) : std::string();
            for (char& ch : ext) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
            if (ext == ".cu8" || ext == ".cs16" || ext == ".cf32") {
                std::string e; double rate = 0.0;
                if (!parseRate(dv("r.rate", "2048000"), rate, e)) { refuseHere(kSource, tr("Make the waterfall"), tr("Sample rate") + ": " + e); return; }
                rq.rawFormat = ext.substr(1); rq.rawRateHz = rate;
            }
            if (!ovJob.start(rq)) { refuseHere(kSource, tr("Make the waterfall"), tr("a picture is being made already")); return; }
            ovState = "running";
            setNote(kSource, tr("Whole-file waterfall") + ": " + tr("started"), kDim);
        }, !ovRun);
        key(v, "w.cancel", tr("Cancel the waterfall"), [this] { ovJob.cancel(); setNote(kSource, tr("Whole-file waterfall") + ": " + tr("cancelling"), kDim); }, ovRun);
        endNote(v, kSource);
    }

    // ================================================================ the models
    std::array<std::shared_ptr<slint::VectorModel<KRow>>, kSecCount> models;

    void publishSection(Sec s, const std::vector<Row>& rows) {
        const std::string g = slk::signatureOf(rows);
        if (g == sig[s] && models[s]) { return; }
        sig[s] = g;
        lastCount[s] = std::to_string(rows.size());
        std::vector<KRow> ks;
        ks.reserve(rows.size());
        for (const auto& r : rows) { ks.push_back(toK(r)); }
        models[s] = std::make_shared<slint::VectorModel<KRow>>(std::move(ks));
        if (env.ui == nullptr) { return; }
        auto& l = env.ui->global<LaneS3>();
        switch (s) {
            case kRecord: l.set_rows_record(models[s]); break;
            case kSchedule: l.set_rows_schedule(models[s]); break;
            case kSource: l.set_rows_source(models[s]); break;
            case kPlay: l.set_rows_play(models[s]); break;
            default: break;
        }
    }

    void rebuildAll() {
        dirty = false;
        using Builder = void (Impl::*)(std::vector<Row>&);
        static const Builder builders[kSecCount] = {&Impl::buildRecord, &Impl::buildSchedule, &Impl::buildSource, &Impl::buildPlay};
        handlers.clear();
        for (int s = 0; s < kSecCount; ++s) {
            std::vector<Row> rows;
            (this->*builders[s])(rows);
            publishSection(static_cast<Sec>(s), rows);
        }
    }

    void act(const std::string& id, const std::string& text) {
        if (!armed.empty() && armed != id) { armed.clear(); noteText[kSchedule].clear(); }
        const auto it = handlers.find(id);
        if (it == handlers.end()) { return; }
        const Handler h = it->second;
        h(text);
        rebuildAll();
    }
};

LaneS3Publisher::LaneS3Publisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
LaneS3Publisher::~LaneS3Publisher() = default;

void LaneS3Publisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneS3>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void LaneS3Publisher::tick(long tickNo) {
    p_->pollOverview();
    if (p_->ovJob.running()) { p_->dirty = true; }   // the progress bar
    if (tickNo - p_->lastPlayTick >= 6 && p_->ps().filePlays) { p_->lastPlayTick = tickNo; p_->dirty = true; }   // the position
    // about twice a second the rows are read again from the facts; at once after any action or answer
    if (p_->dirty || tickNo - p_->lastRebuildTick >= 15) {
        p_->lastRebuildTick = tickNo;
        p_->rebuildAll();
    }
}

void LaneS3Publisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

std::string LaneS3Publisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "s3.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "s3.rows.<section>, not " + sec;
    }
    if (what.rfind("draft.", 0) == 0) { out = p.dv(what.substr(6)); return std::string(); }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    if (what == "ov.state") { out = p.ovState; return std::string(); }
    if (what == "ov.w") { out = std::to_string(p.ovW); return std::string(); }
    if (what == "ov.h") { out = std::to_string(p.ovH); return std::string(); }
    if (what == "play.pos") { out = numberText(p.ps().positionS); return std::string(); }
    if (what == "play.dur") { out = numberText(p.ps().durationS); return std::string(); }
    if (what == "play.loopa") { out = numberText(p.ps().loopStartS); return std::string(); }
    if (what == "play.loopb") { out = numberText(p.ps().loopEndS); return std::string(); }
    if (what == "play.rate") { out = numberText(p.ps().sampleRateHz); return std::string(); }
    if (what == "format.iq") { out = std::to_string(p.ps().recIq); return std::string(); }
    if (what == "format.audio") { out = std::to_string(p.ps().recAudio); return std::string(); }
    return "s3.<sent|refused|answered|note.<section>|rows.<section>|draft.<id>|fact.<name>|play.<pos|dur|loopa|loopb|rate>|format.<iq|audio>>, not " + what;
}

}  // namespace foxsdr::slintlanes3
