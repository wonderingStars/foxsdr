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

    // the rotator's bearing drafts (typed, not sent until "Turn the antenna")
    std::string rotAz = "0", rotEl = "0";
    void realField(std::vector<Row>& v, Sec s, const std::string& id, const std::string& label, const std::string& key, double lo, double hi) {
        field(v, id, label, fact("wirec." + key), [this, s, label, key, lo, hi](const std::string& t) {
            const auto p = parseDegrees(t, lo, hi);
            if (!p) { refuseHere(s, label, cascade::core::formatText(cascade::i18n::tr("A number of degrees from %d to %d."), static_cast<int>(lo), static_cast<int>(hi))); return; }
            char b[32];
            std::snprintf(b, sizeof b, "%.2f", *p);
            setting(s, label, key, b);
        }, cascade::core::formatText(cascade::i18n::tr("%d to %d"), static_cast<int>(lo), static_cast<int>(hi)));
    }
    void rotatorRows(std::vector<Row>& v, Sec S) {
        v.push_back(rHeading(tr("Antenna rotator")));
        const bool on = fact("wirec.rot.enabled") == "1";
        const bool serial = fact("wirec.rot.kind") == "gs232";
        wircTick(v, S, "rot.on", tr("Connect to an antenna rotator"), "rot.enabled", tr("Nothing moves the antenna until you press Turn. Turning it never keys a transmitter."));
        choice(v, "rot.kind", tr("Rotator controller"), {tr("rotctld (network)"), tr("Yaesu GS-232 (serial)")}, serial ? 1 : 0,
               [this, S](int i) { setting(S, tr("Rotator controller"), "rot.kind", i == 1 ? "gs232" : "rotctld"); });
        if (!serial) {
            field(v, "rot.host", tr("rotctld address"), fact("wirec.rot.host"), [this, S](const std::string& t) { setting(S, tr("rotctld address"), "rot.host", trim(t)); }, tr("127.0.0.1 is this computer."));
            wholeField(v, S, "rot.port", tr("rotctld port"), "rot.port", 1, 65535);
        } else {
            field(v, "rot.serial", tr("Rotator serial port"), fact("wirec.rot.serial"), [this, S](const std::string& t) { setting(S, tr("Rotator serial port"), "rot.serial", trim(t)); }, tr("A port name like COM3."));
            choice(v, "rot.baud", tr("Rotator baud rate"), {"1200", "2400", "4800", "9600", "19200"}, std::max(0, std::min(4, [&] { const std::string b = fact("wirec.rot.baud"); return b == "1200" ? 0 : b == "2400" ? 1 : b == "4800" ? 2 : b == "19200" ? 4 : 3; }())),
                   [this, S](int i) { static const char* const kB[] = {"1200", "2400", "4800", "9600", "19200"}; setting(S, tr("Rotator baud rate"), "rot.baud", kB[std::max(0, std::min(4, i))]); });
        }
        choice(v, "rot.azmax", tr("Rotator azimuth range"), {tr("0 to 360"), tr("0 to 450")}, fact("wirec.rot.az_max") == "360" ? 0 : 1,
               [this, S](int i) { setting(S, tr("Rotator azimuth range"), "rot.az_max", i == 0 ? "360" : "450"); });
        realField(v, S, "rot.parkaz", tr("Park azimuth"), "rot.park_az", 0, 450);
        realField(v, S, "rot.parkel", tr("Park elevation"), "rot.park_el", 0, 180);
        field(v, "rot.az", tr("Turn to azimuth"), rotAz, [this, S](const std::string& t) { rotAz = trim(t); dirty = true; (void)S; }, tr("Degrees."));
        field(v, "rot.el", tr("Turn to elevation"), rotEl, [this, S](const std::string& t) { rotEl = trim(t); dirty = true; (void)S; }, tr("Degrees. Many rotators ignore it."));
        key(v, "rot.turn", tr("Turn the antenna"), [this, S] {
            const double azMax = fact("wirec.rot.az_max") == "360" ? 360.0 : 450.0;
            const auto az = parseDegrees(rotAz, 0, azMax), el = parseDegrees(rotEl, 0, 180);
            if (!az || !el) { refuseHere(S, tr("Turn the antenna"), tr("The bearing is an azimuth and an elevation in degrees.")); return; }
            Cmd c;
            c.op = FOXAPP_OP_ROTATOR;
            c.text = rotatorTurnJson(*az, *el);
            send(S, tr("Turn the antenna"), c);
        }, on);
        key(v, "rot.stop", tr("Stop the rotator"), [this, S] { Cmd c; c.op = FOXAPP_OP_ROTATOR; c.text = rotatorActionJson("stop"); send(S, tr("Stop the rotator"), c); }, on);
        key(v, "rot.park", tr("Park the antenna"), [this, S] { Cmd c; c.op = FOXAPP_OP_ROTATOR; c.text = rotatorActionJson("park"); send(S, tr("Park the antenna"), c); }, on);
        if (on) {
            const std::string problem = fact("rot.problem");
            v.push_back(rReadout(tr("Rotator"), !problem.empty() ? problem : (factOn("rot.connected") ? tr("connected") + ", " + fact("rot.scope") : tr("connecting"))));
            if (!fact("rot.az").empty()) { v.push_back(rReadout(tr("Antenna at (azimuth, elevation)"), fact("rot.az") + ", " + fact("rot.el"))); }
        }
        if (on && !serial && !fact("rot.scope").empty() && fact("rot.scope") != "this computer only") { v.push_back(rNote(fact("rot.scope"), kWarn)); }
    }

    // ================================================================ SYSTEM > Outputs
    void buildOut(std::vector<Row>& v) {
        const Sec S = kOut;
        rotatorRows(v, S);
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
        // ---- rtl_tcp server (G3-24)
        v.push_back(rHeading(tr("rtl_tcp server")));
        const bool rtlOn = fact("wirec.srv.rtltcp.enabled") == "1";
        wircTick(v, S, "rtl.on", tr("Serve the radio's samples like rtl_tcp"), "srv.rtltcp.enabled",
                 tr("Programs that read an RTL-SDR over the network can connect and receive this radio's samples. They can retune the radio; nothing else they send is used."));
        wircTick(v, S, "rtl.all", tr("Let other computers connect for rtl_tcp"), "srv.rtltcp.all", tr("Off: only programs on this computer can connect."));
        wholeField(v, S, "rtl.port", tr("rtl_tcp port"), "srv.rtltcp.port", 1024, 65535);
        wholeField(v, S, "rtl.radio", tr("Radio for network streams and control"), "srv.control.radio", 1, 8);
        serverReadouts(v, tr("rtl_tcp"), "srv.rtltcp", rtlOn);
        if (rtlOn && factOn("srv.rtltcp.running")) {
            v.push_back(rReadout(tr("Retunes from the client"), fact("srv.rtltcp.tunes")));
            v.push_back(rReadout(tr("Commands not used"), fact("srv.rtltcp.ignored")));
        }
        if (fact("wirec.srv.rtltcp.all") == "1") { v.push_back(rNote(tr("The rtl_tcp server has no password. Anyone on your network can connect, listen and retune the radio."), kWarn)); }

        // ---- desktop notifications for keyword alerts (G3-37b)
        v.push_back(rHeading(tr("Desktop notifications")));
        wircTick(v, S, "notify.on", tr("Show each alert as a desktop notification"), "alerts.notify",
                 tr("When a keyword alert is raised a notice appears in the notification area of Windows. Alerts are off until you turn them on above."));
        if (fact("wirec.alerts.enabled") == "1") {
            v.push_back(rReadout(tr("Notifications handed to Windows"), fact("alerts.notified")));
            const std::string np = fact("alerts.notify_problem");
            if (!np.empty()) { v.push_back(rNote(np, kErr)); }
        }

        // ---- the program interface: WebSocket and event stream (G3-31)
        v.push_back(rHeading(tr("Program interface")));
        const bool apiOn = fact("wirec.srv.api.enabled") == "1";
        wircTick(v, S, "api.on", tr("Serve the program interface"), "srv.api.enabled",
                 tr("Other programs can read the receiver's status, bookmarks and decoded lines, and retune it, over a WebSocket or an event stream."));
        wircTick(v, S, "api.all", tr("Let other computers connect to the program interface"), "srv.api.all",
                 tr("Off: only programs on this computer can connect. On: they must sign in with the web page's user name and password, which needs a password set for the web page."));
        wholeField(v, S, "api.port", tr("Program interface port"), "srv.api.port", 1024, 65535);
        serverReadouts(v, tr("Program interface"), "srv.api", apiOn);
        if (apiOn && factOn("srv.api.running")) {
            v.push_back(rReadout(tr("WebSockets opened"), fact("srv.api.websockets")));
            v.push_back(rReadout(tr("Event streams opened"), fact("srv.api.streams")));
            v.push_back(rReadout(tr("Retunes from programs"), fact("srv.api.tunes")));
        }
        v.push_back(rNote(tr("A program can read the status, bookmarks and decoded lines and retune the receiver. It cannot transmit, read or write files, change settings or run anything."), kDim));
        if (fact("wirec.srv.api.all") == "1") { v.push_back(rNote(tr("The web page's user name and password are sent without encryption on this connection."), kWarn)); }

        // ---- the MCP server (G3-32)
        v.push_back(rHeading(tr("MCP server")));
        const bool mcpOn = fact("wirec.srv.mcp.enabled") == "1";
        wircTick(v, S, "mcp.on", tr("Serve MCP to programs on this computer"), "srv.mcp.enabled",
                 tr("An assistant program on this computer can read the receiver's status, bookmarks and decoded lines and retune it. It listens on this computer only."));
        wholeField(v, S, "mcp.port", tr("MCP port"), "srv.mcp.port", 1024, 65535);
        masked(v, "mcp.token", tr("MCP token"), [this, S](const std::string& t) { setting(S, tr("MCP token"), "srv.mcp.token", t); },
               tr("Optional. Type it and press Enter. When set, every request must carry it. It is kept in the Windows protected store and not shown again."));
        if (fact("wirec.srv.mcp.token") == "set") {
            v.push_back(rReadout(tr("MCP token"), tr("stored")));
            key(v, "mcp.token.clear", tr("Forget the MCP token"), [this, S] { setting(S, tr("MCP token"), "srv.mcp.token", ""); });
        }
        serverReadouts(v, tr("MCP"), "srv.mcp", mcpOn);
        v.push_back(rNote(tr("MCP offers five tools: status, bookmarks, recent decoded lines, tune and set mode. Nothing else can be done through it."), kDim));
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
