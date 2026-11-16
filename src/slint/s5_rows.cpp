// s5_rows.cpp - see s5_rows.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s5_rows.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanel_logic.hpp"
#include "s5_logic.hpp"

namespace foxsdr::slints5 {

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
using slk::rReadout;
using slk::rTick;
using sll::trim;

// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
double numOf(const std::string& s, double def = 0.0) { return s.empty() ? def : std::atof(s.c_str()); }
}  // namespace

// ---------------------------------------------------------------- facts and drafts
std::string S5Rows::fact(const std::string& name) const { std::string s; if (!env_.fact || !env_.fact("fact." + name, s)) { s.clear(); } return s; }
std::string S5Rows::factText(const std::string& name) const { return fact(name); }
int S5Rows::sel() const { const int s = env_.selectedRadio ? env_.selectedRadio() : 1; return s < 1 ? 1 : s; }
std::string S5Rows::dv(const std::string& id, const std::string& def) const { const auto it = draft_.find(id); return it == draft_.end() ? def : it->second; }
int S5Rows::pv(const std::string& id, int def) const { const auto it = draft_.find(id); return it == draft_.end() ? def : std::atoi(it->second.c_str()); }
bool S5Rows::bv(const std::string& id, bool def) const { const auto it = draft_.find(id); return it == draft_.end() ? def : it->second == "1"; }

// ---------------------------------------------------------------- the one place a command leaves
void S5Rows::send(const std::string& what, const Cmd& k, int kind, std::uint32_t radio) {
    if (!env_.submit) { return; }
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = k.op;
    c.ival[0] = k.i0;
    c.ival[1] = k.i1;
    c.num[0] = k.n0; c.num[1] = k.n1;
    const std::string* longp = nullptr;
    if (k.text.size() < sizeof(c.text)) { std::memcpy(c.text, k.text.data(), k.text.size()); } else { longp = &k.text; }
    const SubmitOutcome o = env_.submit(c, radio, longp);
    ++sent_;
    if (o.ticket == 0 || sll::statusIsRefusal(o.status)) {
        ++refused_;
        setNote(what + ": " + sll::refusalText(o.status, o.message), kErr);
    } else {
        pending_[o.ticket] = {what, kind};
        setNote(what + ": " + tr("sent"), kDim);
    }
}

void S5Rows::refuseHere(const std::string& what, const std::string& why) { ++refused_; setNote(what + ": " + why, kErr); }

void S5Rows::onAnswer(const FoxCommandResult& r) {
    const auto it = pending_.find(r.ticket);
    if (it == pending_.end()) { return; }
    const std::string what = it->second.what;
    const int kind = it->second.kind;
    pending_.erase(it);
    ++answered_;
    const std::string msg = trim(r.message);
    if (r.status == FOXAPI_OK) {
        std::string extra;
        if (kind == kClaim) { extra = claimAnswerText(r.applied[0], r.applied[1]); }
        setNote(what + ": " + (extra.empty() ? tr("done") : extra) + (msg.empty() ? std::string() : " (" + msg + ")"), kGood);
    } else if (r.status == FOXAPI_NO_CHANGE) { setNote(what + ": " + sll::refusalText(r.status, msg), kDim); }
    else { ++refused_; setNote(what + ": " + sll::refusalText(r.status, msg), kErr); }
    dirty_ = true;
}

// ---------------------------------------------------------------- row helpers (each registers the row's handler)
void S5Rows::field(std::vector<Row>& v, const std::string& id, const std::string& label, const std::string& def, const std::string& tip) {
    v.push_back(rField(id, label, dv(id, def), tip));
    handlers_[id] = [this, id](const std::string& t) { draft_[id] = t; dirty_ = true; };
}
void S5Rows::key(std::vector<Row>& v, const std::string& id, const std::string& label, std::function<void()> fn, bool enabled, const std::string& tip) {
    v.push_back(rKey(id, label, enabled, tip));
    handlers_[id] = [fn](const std::string&) { fn(); };
}
void S5Rows::tick(std::vector<Row>& v, const std::string& id, const std::string& label, bool on, std::function<void(bool)> fn, const std::string& tip) {
    v.push_back(rTick(id, label, on, true, tip));
    handlers_[id] = [this, fn](const std::string& t) { fn(t == "1"); dirty_ = true; };
}
void S5Rows::choice(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int pick, std::function<void(int)> fn, const std::string& tip) {
    v.push_back(rChoice(id, label, opts, pick, true, tip));
    handlers_[id] = [this, fn](const std::string& t) { fn(std::atoi(t.c_str())); dirty_ = true; };
}
void S5Rows::choiceD(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int def) {
    choice(v, id, label, opts, pv(id, def), [this, id](int i) { draft_[id] = std::to_string(i); });
}
void S5Rows::tickD(std::vector<Row>& v, const std::string& id, const std::string& label, bool def, const std::string& tip) {
    tick(v, id, label, bv(id, def), [this, id](bool on) { draft_[id] = on ? "1" : "0"; }, tip);
}

// ================================================================ SIGNAL > Source
// The whole panel is folded away until asked for: the Source section is open at every start and a longer body pushes the sections under it off a small window.
std::vector<Row> S5Rows::build() {
    dirty_ = false;
    handlers_.clear();
    std::vector<Row> v;
    const bool more = bv("s.open", false);
    key(v, "s.more", more ? tr("Hide the extra source controls") : tr("More source controls"), [this, more] { draft_["s.open"] = more ? "0" : "1"; }, true,
        tr("A raw I/Q stream over the network, automatic ppm, tuner pooling and the RTL-SDR's direct-sampling choice."));
    if (more) {
        buildRawIq(v);
        buildPpm(v);
        buildPool(v);
        buildDirect(v);
        buildS5b(v);
    }
    if (!note_.empty()) { v.push_back(rNote(note_, noteTone_)); }
    return v;
}

bool S5Rows::act(const std::string& id, const std::string& text) {
    // a key that asked for a second press is disarmed by any other action
    if (!armed_.empty() && armed_ != id) { armed_.clear(); }
    const auto it = handlers_.find(id);
    if (it == handlers_.end()) { return false; }
    const auto h = it->second;
    h(text);
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- G1-05 raw I/Q over TCP or UDP
void S5Rows::buildRawIq(std::vector<Row>& v) {
    v.push_back(rHeading(tr("Raw I/Q stream")));
    v.push_back(rNote(tr("Opens a stream of raw I and Q samples sent over the network (another FoxSDR's network output, rtl_sdr through netcat, a script) as this radio's source.")));
    const auto fields = [this] {
        RawIqFields f;
        f.udp = pv("r.proto", 0);
        f.host = dv("r.host", "127.0.0.1");
        f.port = dv("r.port", "");
        f.format = pv("r.fmt", 0);
        f.rateHz = dv("r.rate", "");
        f.centreHz = dv("r.centre", "0");
        f.header = bv("r.hdr", false);
        f.timeoutMs = dv("r.timeout", "3000");
        return f;
    };
    choiceD(v, "r.proto", tr("How"), {tr("Connect (TCP)"), tr("Listen (UDP)")}, 0);
    const RawIqFields f = fields();
    v.push_back(rNote(rawIqProtocolNote(f)));
    field(v, "r.host", tr("Address"), "127.0.0.1", tr("The computer to connect to, or the local address to listen on"));
    field(v, "r.port", tr("Port"), "", tr("1 to 65535"));
    tickD(v, "r.hdr", tr("The stream carries FoxSDR's header"), false, tr("FoxSDR's own network output sends a small header with the format, rate and centre frequency. Tick this to read them from it."));
    if (!f.header) {
        choiceD(v, "r.fmt", tr("Sample format"), rawIqFormatWords(), 0);
        field(v, "r.rate", tr("Sample rate (Hz)"), "", tr("1000 to 100000000: the stream does not say"));
        field(v, "r.centre", tr("Centre frequency shown (Hz)"), "0", tr("A stream has no tuner: this number is only the label the spectrum shows"));
    }
    field(v, "r.timeout", tr("Silence allowed (ms)"), "3000", tr("100 to 600000: how long the sender may be silent before the radio is reported lost"));
    key(v, "r.open", tr("Open the stream"), [this, fields] {
        const Built b = rawIqOpenText(fields());
        if (!b.ok) { refuseHere(tr("Open the stream"), b.err); return; }
        Cmd c; c.op = FOXAPI_OP_SELECT_SOURCE; c.text = "open-rawiq:" + b.text;
        send(tr("Open the stream"), c);
    }, true, tr("Replaces what this radio is listening to. Nothing connects or listens until you press this key."));
}

// ---------------------------------------------------------------- G1-17 automatic ppm
void S5Rows::buildPpm(std::vector<Row>& v) {
    const std::string p = "radio." + rn() + ".ppmauto.";
    v.push_back(rHeading(tr("Automatic ppm")));
    v.push_back(rNote(ppmReference()));
    const std::string state = fact(p + "state");
    const bool measuring = state == "measuring";
    v.push_back(rReadout(tr("Measurement"), state.empty() ? tr("not measuring") : ppmStateWords(state)));
    if (measuring) { v.push_back(rReadout(tr("Progress"), sll::numberText(std::floor(numOf(fact(p + "progress")) * 100.0 + 0.5)) + " %")); }
    const int readings = std::atoi(fact(p + "readings").c_str());
    const double measured = numOf(fact(p + "ppm"));
    const std::string reading = ppmReading(measured, numOf(fact(p + "uncertainty")), readings);
    if (!reading.empty()) { v.push_back(rReadout(tr("Measured"), reading)); }
    field(v, "m.secs", tr("Measure for (seconds)"), "30", tr("10 to 300; 0 = 30. Only on a station in WFM with its stereo pilot."));
    key(v, "m.start", tr("Measure the crystal error"), [this] {
        double sec = 0; std::string e;
        if (!parsePpmSeconds(dv("m.secs", "30"), sec, e)) { refuseHere(tr("Measure the crystal error"), e); return; }
        Cmd c; c.op = FOXAPP_OP_PPM_MEASURE; c.i0 = 1; c.n0 = sec; send(tr("Measure the crystal error"), c);
    }, !measuring, tr("Tune a strong FM broadcast station in WFM first. The result is a suggestion: nothing is applied by this key."));
    key(v, "m.cancel", tr("Cancel the measurement"), [this] { Cmd c; c.op = FOXAPP_OP_PPM_MEASURE; send(tr("Cancel the measurement"), c); }, measuring);
    const bool done = state == "done" && readings > 0;
    PpmInForce now;
    now.on = fact("ppm.on") == "1";
    now.ppm = numOf(fact("ppm.value"));
    const bool replaces = done && replacesTypedPpm(now, measured);
    key(v, "m.use", armed_ == "m.use" ? tr("Press again to replace the correction") : tr("Use this correction"), [this, measured, replaces] {
        if (replaces) {
            if (armed_ != "m.use") { armed_ = "m.use"; setNote(tr("Use this correction") + ": " + tr("a correction you set is in force; press the key again to replace it with the measured one."), kWarn); return; }
            armed_.clear();
        }
        Cmd c; c.op = FOXAPP_OP_SET_PPM; c.i0 = 1; c.n0 = measured; send(tr("Use this correction"), c);
    }, done, tr("Applies the measured number as this radio's crystal correction. If you have set a correction yourself, the key asks twice before it replaces it."));
    if (now.on) { v.push_back(rReadout(tr("Correction in force"), sll::numberText(now.ppm) + " ppm")); }
}

// ---------------------------------------------------------------- G1-18 tuner pooling
void S5Rows::buildPool(std::vector<Row>& v) {
    v.push_back(rHeading(tr("Tuner pooling")));
    v.push_back(rNote(tr("Asks the open radios for one that is free and tunes it to a frequency: first one whose view already covers it, then one that can move its centre there. A radio that is recording, scanning, locked or stopped is never taken.")));
    field(v, "t.freq", tr("Frequency (MHz)"), "", tr("The frequency to listen on"));
    choiceD(v, "t.mode", tr("Mode"), claimModeWords(), 0);
    field(v, "t.bw", tr("Bandwidth (kHz)"), "0", tr("0 = leave the radio's own"));
    key(v, "t.claim", tr("Find a free radio and tune it"), [this] {
        double hz = 0, bw = 0; std::string e;
        if (!parseClaimMHz(dv("t.freq"), hz, e)) { refuseHere(tr("Find a free radio and tune it"), e); return; }
        if (!parseClaimKHz(dv("t.bw", "0"), bw, e)) { refuseHere(tr("Find a free radio and tune it"), e); return; }
        Cmd c; c.op = FOXAPP_OP_CHANNEL_CLAIM; c.i0 = pv("t.mode", 0); c.n0 = hz; c.n1 = bw;
        send(tr("Find a free radio and tune it"), c, kClaim);
    }, true, tr("The radio stays reserved for this until you release it."));
    for (int n = 1; n <= 8; ++n) {
        if (fact("radio." + std::to_string(n) + ".claimed") != "1") { continue; }
        const std::string label = tr("Release radio") + " " + std::to_string(n);
        key(v, "t.rel" + std::to_string(n), label, [this, n, label] { Cmd c; c.op = FOXAPP_OP_CHANNEL_RELEASE; c.i0 = n; send(label, c); }, true, tr("Lets the pool use this radio again."));
    }
}

// ---------------------------------------------------------------- G1-11 direct sampling
void S5Rows::buildDirect(std::vector<Row>& v) {
    const std::string cur = fact("radio." + rn() + ".directSampling");
    v.push_back(rHeading(tr("Direct sampling")));
    const int idx = directSamplingIndex(cur);
    if (idx < 0) {
        v.push_back(rNote(tr("This radio has no direct-sampling mode. Only an RTL-SDR opened by FoxSDR's own driver has one.")));
        return;
    }
    v.push_back(rNote(tr("Lets an RTL-SDR hear below about 24 MHz by sampling the antenna without its tuner. Automatic does it only where the tuner cannot tune; Never keeps the tuner; the I or Q branch forces it on.")));
    choice(v, "d.mode", tr("Direct sampling"), directSamplingWords(), idx, [this](int i) {
        Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.i0 = i; c.text = "direct_sampling"; send(tr("Direct sampling"), c);
    }, tr("Read back from the radio after each change."));
}

}  // namespace foxsdr::slints5
