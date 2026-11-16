// s4_publish.cpp - see s4_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s4_publish.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "plugin_kit.hpp"
#include "s4_logic.hpp"

namespace foxsdr::slints4 {

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

enum Kind { kPlain = 0, kBookmark, kBookmarkAdd };

struct Cmd {
    std::uint32_t op = 0;
    std::string text;
};
}  // namespace

struct S4Publisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    struct Pending { int sec = 0; std::string what; int kind = kPlain; double hz = 0.0; };
    std::map<std::uint64_t, Pending> pending;
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> sig;
    std::array<std::string, kSecCount> lastCount;
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    long lastRebuildTick = -100;
    bool autoSave = false;                  // "save each hit as a bookmark": the person's switch, off at every start
    AutoBookmarks autoBookmarks;
    int bookmarksSaved = 0, bookmarksSkipped = 0;
    std::string bookmarkReason;             // the engine's own words for the last hit it did not save

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {}

    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    bool factOn(const std::string& name) { return fact(name) == "1"; }

    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    // THE ONE PLACE A COMMAND LEAVES
    void send(Sec s, const std::string& what, const Cmd& k, int kind = kPlain, double num0 = 0.0, double pendingHz = 0.0) {
        if (!env.submit) { return; }
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = k.op;
        c.num[0] = num0;
        const std::string* longp = nullptr;
        if (k.text.size() < sizeof(c.text)) { std::memcpy(c.text, k.text.data(), k.text.size()); } else { longp = &k.text; }
        const SubmitOutcome o = env.submit(c, 0, longp);
        ++sent;
        if (o.ticket == 0 || sll::statusIsRefusal(o.status)) {
            ++refused;
            setNote(s, what + ": " + sll::refusalText(o.status, o.message), kErr);
        } else {
            pending[o.ticket] = {static_cast<int>(s), what, kind, pendingHz};
        }
    }
    void setting(Sec s, const std::string& what, const std::string& key, const std::string& value) {
        Cmd c;
        c.op = FOXAPP_OP_WIREC_SETTING;
        c.text = key + "\x1F" + value;
        send(s, what, c);
    }
    void serverConfig(Sec s, const std::string& what, const std::string& json) {
        Cmd c;
        c.op = FOXAPI_OP_SERVER_CONFIG;
        c.text = json;
        send(s, what, c);
    }
    void refuseHere(Sec s, const std::string& what, const std::string& why) { ++refused; setNote(s, what + ": " + why, kErr); }

    void onAnswer(const FoxCommandResult& r) {
        const auto it = pending.find(r.ticket);
        if (it == pending.end()) { return; }
        const Sec s = static_cast<Sec>(it->second.sec);
        const std::string what = it->second.what;
        const int kind = it->second.kind;
        const double hz = it->second.hz;
        pending.erase(it);
        ++answered;
        const std::string msg = trim(r.message);
        if (kind == kBookmark || kind == kBookmarkAdd) {
            if (r.status == FOXAPI_OK) { ++bookmarksSaved; bookmarkReason.clear(); }
            else if (kind == kBookmark && msg.find("no scan plan") != std::string::npos && !factOn("scanner.listMode")) {
                // a range scan has no plan to take the mode from: the radio is on the signal, so the bookmark is made from what it is tuned to
                char name[96];
                std::snprintf(name, sizeof(name), "%s %.4f MHz", tr("Scan hit").c_str(), hz / 1e6);
                Cmd c;
                c.op = FOXAPI_OP_BOOKMARK_ADD;
                c.text = name;
                send(kScan, tr("Save the signal as a bookmark"), c, kBookmarkAdd, 0.0, hz);
            } else { ++bookmarksSkipped; bookmarkReason = msg; }   // the engine says why (a bookmark already); the row shows its sentence
            dirty = true;
            return;
        }
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
    // a switch of the wirec registry: shows the engine's value, sends the person's
    void wircTick(std::vector<Row>& v, Sec s, const std::string& id, const std::string& label, const std::string& key, const std::string& tip = std::string()) {
        tick(v, id, label, fact("wirec." + key) == "1", [this, s, label, key](bool on) { setting(s, label, key, on ? "1" : "0"); }, tip);
    }
    void wircField(std::vector<Row>& v, Sec s, const std::string& id, const std::string& label, const std::string& key, const std::string& tip = std::string()) {
        field(v, id, label, fact("wirec." + key), [this, s, label, key](const std::string& t) { setting(s, label, key, trim(t)); }, tip);
    }

    // ================================================================ TOOLS > Scanner: hits saved as bookmarks (G2-46)
    void buildScan(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Scan hits")));
        tick(v, "s4.auto", tr("Save each signal the scan stops on as a bookmark"), autoSave, [this](bool on) {
            autoSave = on;
            autoBookmarks.reset();
            if (!on) { bookmarkReason.clear(); }
        }, tr("Adds the frequency to your bookmarks, named Scan hit and the frequency."));
        if (autoSave) {
            v.push_back(rReadout(tr("Saved"), std::to_string(bookmarksSaved)));
            if (bookmarksSkipped > 0) { v.push_back(rReadout(tr("Not saved"), std::to_string(bookmarksSkipped))); }
            if (!bookmarkReason.empty()) { v.push_back(rNote(bookmarkReason, kDim)); }
        }
    }

    // ================================================================ SYSTEM > Outputs: TCI, UDP audio, MQTT, webhook, keyword alerts
    void buildOut(std::vector<Row>& v) {
        const Sec S = kOut;
        v.push_back(rHeading(tr("Network outputs and alerts")));
        v.push_back(rNote(tr("Each output is off until you turn it on. Each one sends to this computer only unless you type another address."), kDim));

        // ---- TCI
        v.push_back(rHeading(tr("TCI interface")));
        const bool tciOn = factOn("tci.enabled");
        const bool tciAll = factOn("tci.bindAll");
        tick(v, "tci.on", tr("Serve the TCI interface"), tciOn, [this, S](bool on) { serverConfig(S, tr("TCI interface"), tciBool("enabled", on)); },
             tr("Receive only. A logging or digital-mode program on this computer can read the frequency and mode and follow the receiver."));
        tick(v, "tci.all", tr("Reachable from other computers"), tciAll, [this, S](bool on) { serverConfig(S, tr("TCI interface"), tciBool("bindAll", on)); },
             tr("Off: only programs on this computer can connect."));
        field(v, "tci.port", tr("TCI port"), fact("tci.port"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 1024, 65535);
            if (!p) { refuseHere(S, tr("TCI port"), tr("The port is a whole number from 1024 to 65535.")); return; }
            serverConfig(S, tr("TCI port"), tciPort(*p));
        }, tr("1024 to 65535"));
        if (tciOn) {
            const std::string refusal = fact("tci.refusal");
            const bool listening = factOn("tci.listening");
            v.push_back(rReadout(tr("TCI"), !refusal.empty() ? refusal : (listening ? tr("listening") + ", " + fact("tci.scope") : tr("starting"))));
            if (listening) { v.push_back(rReadout(tr("TCI clients"), fact("tci.clients"))); }
        }
        if (tciAll) { v.push_back(rNote(tr("The TCI interface has no password. Anyone on your network can connect and read what you receive."), kWarn)); }

        // ---- UDP audio
        v.push_back(rHeading(tr("Audio over UDP")));
        const bool udpOn = factOn("udp.enabled");
        const std::string udpHost = fact("udp.host");
        tick(v, "udp.on", tr("Send the audio over UDP"), udpOn, [this, S](bool on) { serverConfig(S, tr("Audio over UDP"), udpBool("enabled", on)); },
             tr("Sends one radio's audio as raw 16-bit samples, 48000 per second, mono."));
        field(v, "udp.host", tr("Send to address"), udpHost, [this, S](const std::string& t) { serverConfig(S, tr("Send to address"), udpText("host", trim(t))); }, tr("Where the audio is sent. 127.0.0.1 is this computer."));
        field(v, "udp.port", tr("UDP port"), fact("udp.port"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 1, 65535);
            if (!p) { refuseHere(S, tr("UDP port"), tr("The port is a whole number from 1 to 65535.")); return; }
            serverConfig(S, tr("UDP port"), udpInt("port", *p));
        }, tr("1 to 65535"));
        field(v, "udp.radio", tr("Audio radio"), fact("udp.radio"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 1, 8);
            if (!p) { refuseHere(S, tr("Audio radio"), tr("The radio is a number from 1 to 8.")); return; }
            serverConfig(S, tr("Audio radio"), udpInt("radio", *p));
        }, tr("1 to 8"));
        choice(v, "udp.packet", tr("Packet length"), {tr("10 ms"), tr("20 ms")}, fact("udp.packetFrames") == "960" ? 1 : 0,
               [this, S](int i) { serverConfig(S, tr("Packet length"), udpInt("packetFrames", i == 1 ? 960 : 480)); });
        if (udpOn) {
            const std::string refusal = fact("udp.refusal");
            v.push_back(rReadout(tr("Audio"), !refusal.empty() ? refusal : (factOn("udp.sending") ? tr("sending") : tr("starting"))));
            v.push_back(rReadout(tr("Packets sent"), fact("udp.sent")));
        }
        if (leavesThisComputer(udpHost)) { v.push_back(rNote(tr("The audio is sent to another computer without encryption."), kWarn)); }

        // ---- MQTT
        v.push_back(rHeading(tr("MQTT")));
        const bool mqttOn = fact("wirec.out.mqtt.enabled") == "1";
        wircTick(v, S, "mqtt.on", tr("Publish to an MQTT broker"), "out.mqtt.enabled", tr("Sends the lines the decoders print, or only the alerts, to a broker you name."));
        wircField(v, S, "mqtt.host", tr("Broker address"), "out.mqtt.host", tr("127.0.0.1 is this computer."));
        field(v, "mqtt.port", tr("Broker port"), fact("wirec.out.mqtt.port"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 1, 65535);
            if (!p) { refuseHere(S, tr("Broker port"), tr("The port is a whole number from 1 to 65535.")); return; }
            setting(S, tr("Broker port"), "out.mqtt.port", std::to_string(*p));
        }, tr("1 to 65535"));
        wircField(v, S, "mqtt.topic", tr("MQTT topic"), "out.mqtt.topic");
        wircField(v, S, "mqtt.user", tr("MQTT user name"), "out.mqtt.user");
        masked(v, "mqtt.pass", tr("MQTT password"), [this, S](const std::string& t) { setting(S, tr("MQTT password"), "out.mqtt.password", t); }, tr("Type it and press Enter. It is kept in the Windows protected store and not shown again."));
        if (fact("wirec.out.mqtt.password") == "set") {
            v.push_back(rReadout(tr("MQTT password"), tr("stored")));
            key(v, "mqtt.pass.clear", tr("Forget the password"), [this, S] { setting(S, tr("MQTT password"), "out.mqtt.password", ""); });
        }
        choice(v, "mqtt.send", tr("Send"), {tr("Every decoded line"), tr("Only alerts")}, fact("wirec.out.mqtt.send") == "alerts" ? 1 : 0,
               [this, S](int i) { setting(S, tr("Send"), "out.mqtt.send", i == 1 ? "alerts" : "decodes"); });
        if (mqttOn) {
            const std::string problem = fact("out.mqtt.problem");
            v.push_back(rReadout(tr("MQTT"), !problem.empty() ? problem : fact("out.mqtt.state")));
            v.push_back(rReadout(tr("Messages offered"), fact("out.mqtt.offered")));
            const std::string scope = fact("out.mqtt.scope");
            if (!scope.empty() && scope != "this computer only") { v.push_back(rNote(scope, kWarn)); }
        }

        // ---- InfluxDB
        v.push_back(rHeading(tr("InfluxDB")));
        const bool influxOn = fact("wirec.out.influx.enabled") == "1";
        wircTick(v, S, "influx.on", tr("Write the signal level to InfluxDB"), "out.influx.enabled", tr("Writes the selected radio's channel power and frequency as a line of InfluxDB line protocol, at the interval below."));
        wircField(v, S, "influx.host", tr("InfluxDB address"), "out.influx.host", tr("127.0.0.1 is this computer."));
        field(v, "influx.port", tr("InfluxDB port"), fact("wirec.out.influx.port"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 1, 65535);
            if (!p) { refuseHere(S, tr("InfluxDB port"), tr("The port is a whole number from 1 to 65535.")); return; }
            setting(S, tr("InfluxDB port"), "out.influx.port", std::to_string(*p));
        }, tr("1 to 65535"));
        choice(v, "influx.mode", tr("InfluxDB link"), {tr("HTTP write"), tr("UDP")}, fact("wirec.out.influx.mode") == "udp" ? 1 : 0,
               [this, S](int i) { setting(S, tr("InfluxDB link"), "out.influx.mode", i == 1 ? "udp" : "http"); });
        wircField(v, S, "influx.org", tr("InfluxDB organisation"), "out.influx.org");
        wircField(v, S, "influx.bucket", tr("InfluxDB bucket"), "out.influx.bucket");
        masked(v, "influx.token", tr("InfluxDB token"), [this, S](const std::string& t) { setting(S, tr("InfluxDB token"), "out.influx.token", t); }, tr("Type it and press Enter. It is kept in the Windows protected store and not shown again."));
        if (fact("wirec.out.influx.token") == "set") {
            v.push_back(rReadout(tr("InfluxDB token"), tr("stored")));
            key(v, "influx.token.clear", tr("Forget the token"), [this, S] { setting(S, tr("InfluxDB token"), "out.influx.token", ""); });
        }
        field(v, "influx.every", tr("Seconds between writes"), fact("wirec.out.influx.every_s"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 1, 3600);
            if (!p) { refuseHere(S, tr("Seconds between writes"), tr("A whole number from 1 to 3600.")); return; }
            setting(S, tr("Seconds between writes"), "out.influx.every_s", std::to_string(*p));
        }, tr("1 to 3600"));
        if (influxOn) {
            const std::string problem = fact("out.influx.problem");
            v.push_back(rReadout(tr("InfluxDB"), !problem.empty() ? problem : tr("on")));
            v.push_back(rReadout(tr("Written"), fact("out.influx.ok")));
            const std::string scope = fact("out.influx.scope");
            if (!scope.empty() && scope != "this computer only") { v.push_back(rNote(scope, kWarn)); }
        }

        // ---- webhook
        v.push_back(rHeading(tr("Web address")));
        const bool hookOn = fact("wirec.out.webhook.enabled") == "1";
        wircTick(v, S, "hook.on", tr("Post to a web address"), "out.webhook.enabled", tr("Sends the lines the decoders print, or only the alerts, to an address you name."));
        masked(v, "hook.url", tr("Web address to post to"), [this, S](const std::string& t) { setting(S, tr("Web address to post to"), "out.webhook.url", t); },
               tr("https://... anywhere, or http://127.0.0.1... on this computer. It may hold a secret, so it is kept in the Windows protected store and not shown again."));
        if (fact("wirec.out.webhook.url") == "set") {
            v.push_back(rReadout(tr("Web address to post to"), tr("stored")));
            key(v, "hook.url.clear", tr("Forget the address"), [this, S] { setting(S, tr("Web address to post to"), "out.webhook.url", ""); });
        }
        choice(v, "hook.send", tr("Send"), {tr("Every decoded line"), tr("Only alerts")}, fact("wirec.out.webhook.send") == "alerts" ? 1 : 0,
               [this, S](int i) { setting(S, tr("Send"), "out.webhook.send", i == 1 ? "alerts" : "decodes"); });
        if (hookOn) {
            const std::string problem = fact("out.webhook.problem");
            if (!problem.empty()) { v.push_back(rReadout(tr("Web address"), problem)); }
            v.push_back(rReadout(tr("Posted"), fact("out.webhook.ok")));
            v.push_back(rReadout(tr("Failed"), fact("out.webhook.failed")));
            const std::string scope = fact("out.webhook.scope");
            if (!scope.empty()) { v.push_back(rNote(scope, scope == "this computer only" || scope.find("encrypted") != std::string::npos ? kDim : kWarn)); }
        }

        // ---- keyword alerts
        v.push_back(rHeading(tr("Keyword alerts")));
        const bool alertsOn = fact("wirec.alerts.enabled") == "1";
        wircTick(v, S, "alerts.on", tr("Raise an alert when a decoded line holds a keyword"), "alerts.enabled", tr("Checks every line the decoders print. Lines of a private plugin are never checked."));
        field(v, "alerts.words", tr("Keywords, separated by commas"), keywordsToLine(fact("wirec.alerts.keywords")), [this, S](const std::string& t) { setting(S, tr("Keywords"), "alerts.keywords", lineToKeywords(t)); },
              tr("Up to 64 keywords or phrases. Capital letters do not matter."));
        field(v, "alerts.gap", tr("Seconds before the same keyword counts again"), fact("wirec.alerts.gap_s"), [this, S](const std::string& t) {
            const auto p = parseWhole(t, 0, 3600);
            if (!p) { refuseHere(S, tr("Seconds before the same keyword counts again"), tr("A whole number from 0 to 3600.")); return; }
            setting(S, tr("Seconds before the same keyword counts again"), "alerts.gap_s", std::to_string(*p));
        }, tr("0 to 3600"));
        if (alertsOn) {
            const std::string problem = fact("alerts.problem");
            if (!problem.empty()) { v.push_back(rNote(problem, kErr)); }
            v.push_back(rReadout(tr("Alerts raised"), fact("alerts.raised")));
            const std::string recent = fact("alerts.recent");
            std::vector<std::string> lines;
            std::size_t from = 0;
            while (from <= recent.size() && !recent.empty()) {
                std::size_t to = recent.find('\n', from);
                if (to == std::string::npos) { to = recent.size(); }
                if (to > from) { lines.push_back(recent.substr(from, to - from)); }
                from = to + 1;
            }
            const std::size_t first = lines.size() > 5 ? lines.size() - 5 : 0;
            for (std::size_t i = lines.size(); i > first; --i) { v.push_back(rNote(lines[i - 1], kGood)); }
        }
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
        auto& l = env.ui->global<LaneS4>();
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

S4Publisher::S4Publisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
S4Publisher::~S4Publisher() = default;

void S4Publisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneS4>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void S4Publisher::tick(long tickNo) {
    Impl& p = *p_;
    if (p.env.scanner) {
        const auto st = p.env.scanner();
        if (const std::optional<double> hz = p.autoBookmarks.update(p.autoSave, st.first, st.second)) {
            Cmd c;
            c.op = FOXAPP_OP_SCAN_SAVE_HIT;
            p.send(kScan, tr("Save the signal as a bookmark"), c, kBookmark, *hz, *hz);
        }
    }
    if (p.dirty || tickNo - p.lastRebuildTick >= 15) {
        p.lastRebuildTick = tickNo;
        p.rebuildAll();
    }
}

void S4Publisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

std::string S4Publisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what == "saved") { out = std::to_string(p.bookmarksSaved); return std::string(); }
    if (what == "skipped") { out = std::to_string(p.bookmarksSkipped); return std::string(); }
    if (what == "auto") { out = p.autoSave ? "1" : "0"; return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "s4.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "s4.rows.<section>, not " + sec;
    }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    return "s4.<sent|refused|answered|saved|skipped|auto|note.<section>|rows.<section>|fact.<name>>, not " + what;
}

}  // namespace foxsdr::slints4
