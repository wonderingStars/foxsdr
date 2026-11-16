// s4b_publish.cpp - see s4b_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s4b_publish.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "plugin_kit.hpp"
#include "s4_logic.hpp"
#include "s4b_logic.hpp"

namespace foxsdr::slints4b {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
using slk::Row;
using slk::kDim;
using slk::kErr;
using slk::kGood;
using slk::kWarn;
using slk::rField;
using slk::rHeading;
using slk::rKey;
using slk::rNote;
using slk::rChoice;
using slk::rMasked;
using slk::rReadout;
using slk::rTick;
using sll::trim;

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

enum Sec { kScan = 0, kOut, kSecCount };
const char* const kSecName[kSecCount] = {"scan", "outputs"};

struct Cmd {
    std::uint32_t op = 0;
    int ival0 = 0;
    std::string text;
};
}  // namespace

struct S4bPublisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    struct Pending { int sec = 0; std::string what; };
    std::map<std::uint64_t, Pending> pending;
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> sig;
    std::array<std::string, kSecCount> lastCount;
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    long lastRebuildTick = -100;

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {}

    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    bool factOn(const std::string& name) { return fact(name) == "1"; }

    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    // THE ONE PLACE A COMMAND LEAVES
    void send(Sec s, const std::string& what, const Cmd& k) {
        if (!env.submit) { return; }
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = k.op;
        c.ival[0] = k.ival0;
        const std::string* longp = nullptr;
        if (k.text.size() < sizeof(c.text)) { std::memcpy(c.text, k.text.data(), k.text.size()); } else { longp = &k.text; }
        const SubmitOutcome o = env.submit(c, 0, longp);
        ++sent;
        if (o.ticket == 0 || sll::statusIsRefusal(o.status)) {
            ++refused;
            setNote(s, what + ": " + sll::refusalText(o.status, o.message), kErr);
        } else {
            pending[o.ticket] = {static_cast<int>(s), what};
        }
    }
    void setting(Sec s, const std::string& what, const std::string& key, const std::string& value) {
        Cmd c;
        c.op = FOXAPP_OP_WIREC_SETTING;
        c.text = key + "\x1F" + value;
        send(s, what, c);
    }
    void refuseHere(Sec s, const std::string& what, const std::string& why) { ++refused; setNote(s, what + ": " + why, kErr); }

    void onAnswer(const FoxCommandResult& r) {
        const auto it = pending.find(r.ticket);
        if (it == pending.end()) { return; }
        const Sec s = static_cast<Sec>(it->second.sec);
        const std::string what = it->second.what;
        pending.erase(it);
        ++answered;
        const std::string msg = trim(r.message);
        if (r.status == FOXAPI_OK || r.status == FOXAPI_NO_CHANGE) { if (!noteText[s].empty() && noteTone[s] == kErr) { noteText[s].clear(); } }
        else { ++refused; setNote(s, what + ": " + sll::refusalText(r.status, msg), kErr); }
        dirty = true;
    }

    // ---------------------------------------------------------------- row helpers
    void field(std::vector<Row>& v, const std::string& id, const std::string& label, const std::string& value, std::function<void(const std::string&)> fn, const std::string& tip = std::string()) {
        v.push_back(rField(id, label, value, tip));
        handlers[id] = [fn](const std::string& t) { fn(t); };
    }
    void masked(std::vector<Row>& v, const std::string& id, const std::string& label, std::function<void(const std::string&)> fn, const std::string& tip = std::string()) {
        v.push_back(rMasked(id, label, std::string(), tip));   // never filled from anywhere: what is typed is sent once
        handlers[id] = [fn](const std::string& t) { if (!t.empty()) { fn(t); } };
    }
    void key(std::vector<Row>& v, const std::string& id, const std::string& label, std::function<void()> fn, bool enabled = true, const std::string& tip = std::string()) {
        v.push_back(rKey(id, label, enabled, tip));
        handlers[id] = [fn](const std::string&) { fn(); };
    }
    void tick(std::vector<Row>& v, const std::string& id, const std::string& label, bool on, std::function<void(bool)> fn, const std::string& tip = std::string()) {
        v.push_back(rTick(id, label, on, true, tip));
        handlers[id] = [fn](const std::string& t) { fn(t == "1"); };
    }
    void choice(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int pick, std::function<void(int)> fn, const std::string& tip = std::string()) {
        v.push_back(rChoice(id, label, opts, pick, true, tip));
        handlers[id] = [fn](const std::string& t) { fn(std::atoi(t.c_str())); };
    }
    void endNote(std::vector<Row>& v, Sec s) { if (!noteText[s].empty()) { v.push_back(rNote(noteText[s], noteTone[s])); } }

    // ================================================================ TOOLS > Scanner: the locked-out frequencies (G2-43)
    void buildScan(std::vector<Row>& v) {
        const Sec S = kScan;
        v.push_back(rHeading(tr("Locked-out frequencies")));
        // what the engine holds NOW: the ones the scan was told to skip for a while or for good ("Lock out for good", "Lock out 10 min")
        const std::vector<Lockout> now = parseLockouts(fact("scan.lockouts"), 200);
        if (now.empty()) { v.push_back(rNote(tr("No frequency is locked out right now."), kDim)); }
        else {
            const std::size_t shown = std::min<std::size_t>(now.size(), 12);
            for (std::size_t i = 0; i < shown; ++i) { v.push_back(rReadout(tr("Locked out"), lockoutLine(now[i]))); }
            if (now.size() > shown) { v.push_back(rNote(cascade::core::formatText(cascade::i18n::tr("and %d more"), static_cast<int>(now.size() - shown)), kDim)); }
        }
        // what is kept with the configuration (the engine saves it and the scan skips it at every start)
        field(v, "s4b.locked", tr("Always skip these frequencies (MHz, separated by commas)"), hzListToMhzLine(fact("scan.locked")), [this, S](const std::string& t) {
            const HzListResult r = parseMhzList(t);
            if (!r.problem.empty()) { refuseHere(S, tr("Always skip these frequencies"), r.problem); return; }
            Cmd c;
            c.op = FOXAPP_OP_SCANNER_EXTRAS;
            c.ival0 = 5;
            c.text = hzListToWire(r.hz);
            send(S, tr("Always skip these frequencies"), c);
        }, tr("A scan does not stop on these. Up to 1000. Leave it empty to skip none."));
        endNote(v, S);
    }

    // a switch of the wirec registry: shows the engine's value, sends the person's
    void wircTick(std::vector<Row>& v, Sec s, const std::string& id, const std::string& label, const std::string& key, const std::string& tip = std::string()) {
        tick(v, id, label, fact("wirec." + key) == "1", [this, s, label, key](bool on) { setting(s, label, key, on ? "1" : "0"); }, tip);
    }
    void wholeField(std::vector<Row>& v, Sec s, const std::string& id, const std::string& label, const std::string& key, int lo, int hi) {
        field(v, id, label, fact("wirec." + key), [this, s, label, key, lo, hi](const std::string& t) {
            const auto p = foxsdr::slints4::parseWhole(t, lo, hi);
            if (!p) { refuseHere(s, label, cascade::core::formatText(cascade::i18n::tr("A whole number from %d to %d."), lo, hi)); return; }
            setting(s, label, key, std::to_string(*p));
        }, cascade::core::formatText(cascade::i18n::tr("%d to %d"), lo, hi));
    }
    // The state of one server: its own sentence when it could not start, else "listening", with its scope and clients.
    void serverReadouts(std::vector<Row>& v, const std::string& name, const std::string& fx, bool on) {
        if (!on) { return; }
        const std::string problem = fact(fx + ".problem");
        const bool running = factOn(fx + ".running");
        v.push_back(rReadout(name, !problem.empty() ? problem : (running ? tr("listening") + ", " + fact(fx + ".scope") : tr("starting"))));
        if (running) { v.push_back(rReadout(tr("Connected now"), fact(fx + ".clients"))); }
    }

    // ================================================================ SYSTEM > Outputs
    void buildOut(std::vector<Row>& v) {
        const Sec S = kOut;
        // ---- audio over TCP (G3-22)
        v.push_back(rHeading(tr("Audio over TCP")));
        const bool tcpOn = fact("wirec.srv.tcpaudio.enabled") == "1";
        wircTick(v, S, "tcpa.on", tr("Serve the audio over TCP"), "srv.tcpaudio.enabled", tr("A program that connects receives one radio's audio as raw 16-bit samples, 48000 per second, mono: the same as the UDP audio."));
        wircTick(v, S, "tcpa.all", tr("Let other computers connect for the audio"), "srv.tcpaudio.all", tr("Off: only programs on this computer can connect."));
        wholeField(v, S, "tcpa.port", tr("TCP audio port"), "srv.tcpaudio.port", 1024, 65535);
        wholeField(v, S, "tcpa.radio", tr("TCP audio radio"), "srv.tcpaudio.radio", 1, 8);
        choice(v, "tcpa.packet", tr("TCP audio packet length"), {tr("10 ms a write"), tr("20 ms a write")}, fact("wirec.srv.tcpaudio.packet") == "960" ? 1 : 0,
               [this, S](int i) { setting(S, tr("TCP audio packet length"), "srv.tcpaudio.packet", i == 1 ? "960" : "480"); });
        serverReadouts(v, tr("TCP audio"), "srv.tcpaudio", tcpOn);
        if (fact("wirec.srv.tcpaudio.all") == "1") { v.push_back(rNote(tr("The audio has no password. Anyone on your network can connect and listen."), kWarn)); }
        endNote(v, S);
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
        auto& l = env.ui->global<LaneS4b>();
        switch (s) {
            case kScan: l.set_rows_scan(models[s]); break;
            case kOut: l.set_rows_outputs(models[s]); break;
            default: break;
        }
    }

    void rebuildAll() {
        dirty = false;
        handlers.clear();
        {
            std::vector<Row> rows;
            buildScan(rows);
            publishSection(kScan, rows);
        }
        {
            std::vector<Row> rows;
            buildOut(rows);
            publishSection(kOut, rows);
        }
    }

    void act(const std::string& id, const std::string& text) {
        const auto it = handlers.find(id);
        if (it == handlers.end()) { return; }
        const Handler h = it->second;
        h(text);
        rebuildAll();
    }
};

S4bPublisher::S4bPublisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
S4bPublisher::~S4bPublisher() = default;

void S4bPublisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneS4b>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void S4bPublisher::tick(long tickNo) {
    Impl& p = *p_;
    if (p.dirty || tickNo - p.lastRebuildTick >= 15) {
        p.lastRebuildTick = tickNo;
        p.rebuildAll();
    }
}

void S4bPublisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

std::string S4bPublisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "s4b.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "s4b.rows.<section>, not " + sec;
    }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    return "s4b.<sent|refused|answered|note.<section>|rows.<section>|fact.<name>>, not " + what;
}

}  // namespace foxsdr::slints4b
