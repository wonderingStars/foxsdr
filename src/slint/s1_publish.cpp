// s1_publish.cpp - see s1_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s1_publish.hpp"
#include "safe_text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "plugin_kit.hpp"
#include "s1_logic.hpp"

namespace foxsdr::slints1 {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
using slk::Row;
using slk::kDim;
using slk::kErr;
using slk::kGood;
using slk::rChoice;
using slk::rField;
using slk::rKey;
using slk::rNote;
using slk::rReadout;
using slk::rSlider;
using slk::rTick;
using sll::numberText;
using sll::trim;

// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
slint::SharedString ss(const std::string& s) { return foxsdr::slintsafe::shared(s, __FILE__); }

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

// One row's whole content as a short string: what decides whether the row on screen is to be set again.
std::string rowSig(const Row& r) {
    std::string s = std::to_string(r.kind) + '\x1f' + r.id + '\x1f' + r.label + '\x1f' + r.value + '\x1f' + r.note + '\x1f' + (r.on ? '1' : '0') + (r.enabled ? '1' : '0') + '\x1f' + std::to_string(r.pick) + '\x1f' + std::to_string(r.tone) + '\x1f';
    char b[32];
    std::snprintf(b, sizeof(b), "%.4f", static_cast<double>(r.level));
    s += b;
    for (const auto& o : r.options) { s += '\x1e'; s += o; }
    return s;
}

enum Sec { kSource = 0, kRadio, kAudio, kSecCount };
const char* const kSecName[kSecCount] = {"source", "radio", "audio"};

struct Cmd {
    std::uint32_t op = 0;
    long long i0 = 0, i1 = 0;
    double n[4] = {0, 0, 0, 0};
};
Cmd cmd(std::uint32_t op, long long i0 = 0, long long i1 = 0, double n0 = 0, double n1 = 0, double n2 = 0) {
    Cmd c; c.op = op; c.i0 = i0; c.i1 = i1; c.n[0] = n0; c.n[1] = n1; c.n[2] = n2;
    return c;
}

std::string hzText(double hz) { char b[48]; std::snprintf(b, sizeof(b), "%.0f Hz", hz); return b; }
std::string dbText(double db) { char b[48]; std::snprintf(b, sizeof(b), "%+.1f dB", db); return b; }
std::string dbPlain(double db) { char b[48]; std::snprintf(b, sizeof(b), "%.0f dB", db); return b; }
std::string msText(double ms) { char b[48]; std::snprintf(b, sizeof(b), "%.0f ms", ms); return b; }
std::string signedHz(double hz) { char b[48]; std::snprintf(b, sizeof(b), "%+.0f Hz", hz); return b; }
std::string dbOne(double db) { char b[48]; std::snprintf(b, sizeof(b), "%.1f dB", db); return b; }
std::string ratioText(double v) { char b[48]; std::snprintf(b, sizeof(b), "%.2f", v); return b; }
}  // namespace

struct S1Publisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    std::map<std::string, std::string> draft;          // what the person typed and has not sent (never saved); also which groups are open ("g.<id>")
    struct Pending { int sec = 0; std::string what; };
    std::map<std::uint64_t, Pending> pending;          // ticket -> section, what it was
    std::map<std::uint64_t, std::vector<std::string>> pendingDraft;   // ticket -> the fields whose typed text is dropped once the engine has taken it
    struct Held { double value = 0.0; long tick = -1000; };
    std::map<std::string, Held> held;                  // a slider's last sent figure: shown for a moment while the engine's state catches up
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> lastCount;
    FoxReceiverState st{};
    bool haveState = false;
    int lastSel = 0;
    long tickNo = 0;
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    long lastRebuildTick = -100;

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) { st.structSize = sizeof(st); }

    // ---------------------------------------------------------------- facts and drafts
    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    int sel() { const int s = env.selectedRadio ? env.selectedRadio() : 1; return s < 1 ? 1 : s; }
    std::string rn() { return std::to_string(sel()); }
    std::string dv(const std::string& id, const std::string& def = std::string()) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second; }
    bool open(const std::string& g) const { const auto it = draft.find("g." + g); return it != draft.end() && it->second == "1"; }

    // ---------------------------------------------------------------- the one place a command leaves
    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    void send(Sec s, const std::string& what, const Cmd& k, const std::vector<std::string>& draftsToDrop = {}) {
        if (!env.submit) { return; }
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = k.op;
        c.ival[0] = k.i0;
        c.ival[1] = k.i1;
        for (int i = 0; i < 4; ++i) { c.num[i] = k.n[i]; }
        const SubmitOutcome o = env.submit(c, 0, nullptr);
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
    // The section's last row is always a note (empty until something was sent), so a first answer never changes the list's shape and a slider being dragged is not rebuilt under the pointer.
    void endNote(std::vector<Row>& v, Sec s) { placeNote(v, noteText[s], noteTone[s]); }

    // The key that folds a group away: lit while the group is open. Returns whether it is.
    bool fold(std::vector<Row>& v, const std::string& id, const std::string& label, const std::string& tip) {
        const bool o = open(id);
        v.push_back(rKey("g." + id, label, true, tip, o));
        handlers["g." + id] = [this, id, o](const std::string&) { draft["g." + id] = o ? "0" : "1"; };
        return o;
    }
    void key(std::vector<Row>& v, const std::string& id, const std::string& label, std::function<void()> fn, bool enabled = true, const std::string& tip = std::string()) {
        v.push_back(rKey(id, label, enabled, tip));
        handlers[id] = [fn](const std::string&) { fn(); };
    }
    void tickRow(std::vector<Row>& v, const std::string& id, const std::string& label, bool on, std::function<void(bool)> fn, bool enabled = true, const std::string& tip = std::string()) {
        v.push_back(rTick(id, label, on, enabled, tip));
        handlers[id] = [fn](const std::string& t) { fn(t == "1"); };
    }
    void choice(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int pick, std::function<void(int)> fn, const std::string& tip = std::string()) {
        v.push_back(rChoice(id, label, opts, pick, true, tip));
        handlers[id] = [fn, n = static_cast<int>(opts.size())](const std::string& t) { const int i = std::atoi(t.c_str()); if (i >= 0 && i < n) { fn(i); } };
    }
    // A slider over `r`, kept to `step`. What it sends is dropped when it is the figure it sent a moment ago, and the figure is shown for a moment while the engine's state catches up.
    void slider(std::vector<Row>& v, const std::string& id, const std::string& label, const Range& r, double step, double engineValue, std::string (*text)(double),
                std::function<void(double)> fn, const std::string& tip = std::string()) {
        double shown = engineValue;
        const auto h = held.find(id);
        if (h != held.end() && tickNo - h->second.tick < 15) { shown = h->second.value; }
        v.push_back(rSlider(id, label, toLevel(r, shown), text(shown), true, tip));
        handlers[id] = [this, id, r, step, fn](const std::string& t) {
            const double value = fromLevelStep(r, std::atof(t.c_str()), step);
            const auto h2 = held.find(id);
            if (h2 != held.end() && tickNo - h2->second.tick < 15 && h2->second.value == value) { return; }
            held[id] = {value, tickNo};
            fn(value);
        };
    }
    // A field that sends when the person commits it: what is typed stays (as the draft) until the engine has taken it, then the field shows the engine's figure again.
    void field(std::vector<Row>& v, const std::string& id, const std::string& label, const std::string& engineText, std::function<void(const std::string&)> commit, const std::string& tip = std::string()) {
        v.push_back(rField(id, label, dv(id, engineText), tip));
        handlers[id] = [this, id, commit](const std::string& t) { draft[id] = t; commit(t); };
    }
    // A field that only keeps what is typed, for a key to send.
    void typed(std::vector<Row>& v, const std::string& id, const std::string& label, const std::string& engineText, const std::string& tip = std::string()) {
        v.push_back(rField(id, label, dv(id, engineText), tip));
        handlers[id] = [this, id](const std::string& t) { draft[id] = t; };
    }

    // ================================================================ SIGNAL > Source: the I/Q correction (G1-46a, G1-46b, G1-46c)
    void buildSource(std::vector<Row>& v) {
        if (!haveState) { return; }
        if (fold(v, "iq", tr("I/Q correction"), tr("Corrects the samples the radio gives before anything else hears them: the DC offset, the unequal gain and phase of the two paths, or the two paths the wrong way round. All off leaves the samples untouched."))) {
            const unsigned mask = st.iqCorrectMask;
            const auto setBit = [this, mask](unsigned bit, bool on, const std::string& what) {
                const unsigned m = on ? (mask | bit) : (mask & ~bit);
                send(kSource, what, cmd(FOXAPI_OP_SET_IQ_CORRECT, static_cast<long long>(m)));
            };
            tickRow(v, "i.dc", tr("Remove the DC offset"), (mask & kIqDc) != 0u, [setBit](bool on) { setBit(kIqDc, on, tr("Remove the DC offset")); }, true,
                    tr("Takes out the constant offset some receivers add, which shows as a spike in the middle of the spectrum."));
            tickRow(v, "i.bal", tr("Balance gain and phase"), (mask & kIqBalance) != 0u, [setBit](bool on) { setBit(kIqBalance, on, tr("Balance gain and phase")); }, true,
                    tr("Cancels the mirror image a receiver makes when its two paths differ in gain or phase."));
            tickRow(v, "i.swap", tr("Swap I and Q (any source)"), (mask & kIqSwap) != 0u, [setBit](bool on) { setBit(kIqSwap, on, tr("Swap I and Q (any source)")); }, true,
                    tr("Turns the spectrum over, for a source whose two paths arrive the wrong way round."));
            const std::string ir = imageRejectionText(st.iqImageRejectionDb);
            v.push_back(rReadout(tr("Image rejection"), ir.empty() ? tr("not measured") : ir));
        }
        endNote(v, kSource);
    }

    // ================================================================ SIGNAL > Radio
    void buildMeter(std::vector<Row>& v) {
        if (!fold(v, "meter", tr("Signal meter units"), tr("What the signal strength reading is lettered in: the level in dB of full scale, S units, or dBm."))) { return; }
        choice(v, "m.unit", tr("Meter unit"), meterUnitNames(), static_cast<int>(st.meterUnits), [this](int i) { send(kRadio, tr("Meter unit"), cmd(FOXAPI_OP_SET_METER_UNITS, i)); },
               tr("The bar keeps its own scale; this changes the figure beside it. S units and dBm are an estimate until a calibration of this radio covers the tuned frequency."));
        v.push_back(rReadout(tr("Meter now"), meterReading(st.meterUnits, st.signalDb, st.signalDbm, st.sUnits, st.calibrated != 0u)));
        if (st.meterUnits != FOXAPI_METER_DBFS && st.calibrated == 0u) { v.push_back(rNote(tr("No calibration covers this frequency, so the figure is an estimate (marked ~)."))); }
    }

    void buildCw(std::vector<Row>& v) {
        if (!fold(v, "cw", tr("CW pitch and peak filter"), tr("The pitch a carrier on the tuned frequency beats at in CW mode, and a narrow peak filter centred on it."))) { return; }
        slider(v, "w.pitch", tr("CW pitch"), kCwPitch, 1.0, st.cwPitchHz, hzText, [this](double hz) { send(kRadio, tr("CW pitch"), cmd(FOXAPI_OP_SET_CW_PITCH, 0, 0, hz)); },
               tr("300 to 1200 Hz: the tone you hear for a carrier on the tuned frequency in CW mode."));
        const bool on = st.cwPeakOn != 0u;
        const double width = st.cwPeakWidthHz, boost = st.cwPeakBoostDb;
        tickRow(v, "w.peak", tr("CW peak filter"), on, [this, width, boost](bool o) { send(kRadio, tr("CW peak filter"), cmd(FOXAPI_OP_SET_CW_PEAK, o ? 1 : 0, 1, width, boost)); }, true,
                tr("Lifts a narrow band around the CW pitch so a weak signal stands out of the noise."));
        slider(v, "w.width", tr("Peak width"), kCwPeakWidth, 10.0, width, hzText, [this, on, boost](double w) { send(kRadio, tr("Peak width"), cmd(FOXAPI_OP_SET_CW_PEAK, on ? 1 : 0, 1, w, boost)); }, tr("30 to 500 Hz"));
        slider(v, "w.boost", tr("Peak boost"), kCwPeakBoost, 1.0, boost, dbPlain, [this, on, width](double b) { send(kRadio, tr("Peak boost"), cmd(FOXAPI_OP_SET_CW_PEAK, on ? 1 : 0, 1, width, b)); }, tr("0 to 24 dB"));
    }

    void buildPassband(std::vector<Row>& v) {
        if (!fold(v, "pass", tr("Filter width and shape"), tr("Preset widths for the mode you are in, and how steep the channel filter's edges are."))) { return; }
        const unsigned mode = st.demodMode;
        v.push_back(rReadout(tr("Filter width now"), widthText(st.bandwidthHz)));
        const std::vector<double> presets = widthPresets(mode);
        const int at = presetIndexOf(presets, st.bandwidthHz);
        if (isFmMode(mode)) {
            choice(v, "p.fm", tr("FM width"), fmWidthNames(), at, [this](int i) { send(kRadio, tr("FM width"), cmd(FOXAPP_OP_SET_FM_WIDTH, i)); },
                   tr("The usual widths of an FM channel, from a two-way channel to a super-wide one. The widest the radio can show is held to 0.9 of its channel rate."));
        } else {
            std::vector<std::string> names;
            for (const double w : presets) { names.push_back(tr("Width") + " " + widthText(w)); }
            choice(v, "p.width", tr("Width preset"), names, at, [this, presets](int i) { send(kRadio, tr("Width preset"), cmd(FOXAPI_OP_SET_BANDWIDTH, 0, 0, presets[static_cast<std::size_t>(i)])); },
                   tr("Widths for the mode you are in. The radio holds the width at 3 kHz or more."));
        }
        choice(v, "p.shape", tr("Filter shape"), shapeNames(), static_cast<int>(st.filterShape), [this](int i) { send(kRadio, tr("Filter shape"), cmd(FOXAPI_OP_SET_FILTER_SHAPE, i)); },
               tr("How square the channel filter's edges are: sharp cuts a neighbour closer, soft rings less."));
        if (st.filterShape != 0u && st.filterShapeActive == 0u) { v.push_back(rNote(tr("This shape is not in the filter at this width: it needs a narrow channel stage."))); }
    }

    // ---- the passband's own edges and the IF shift (G1-35, G1-34)
    void sendPass(bool on, double low, double high, double shift, const std::string& what, const std::vector<std::string>& drops) {
        send(kRadio, what, cmd(FOXAPP_OP_SET_PASSBAND, on ? 1 : 0, 1, low, high, shift), drops);
    }
    void buildEdges(std::vector<Row>& v) {
        if (!fold(v, "edges", tr("Passband edges and shift"), tr("The two edges of the filter you hear through set apart, and the whole band moved up or down (IF shift). They work in USB, LSB, CW and AM and are in addition to the width above."))) { return; }
        const unsigned mode = st.demodMode;
        const bool has = hasPassband(mode);
        const PassbandFact pf = parsePassbandFact(fact("radio." + rn() + ".passband"));
        const bool on = pf.ok && pf.on;
        double low = 300.0, high = 2700.0;
        defaultEdges(mode, st.cwPitchHz, low, high);
        if (on) { low = pf.low; high = pf.high; }
        const double shift = pf.ok ? pf.shift : 0.0;
        if (!has) { v.push_back(rNote(tr("The edges work in USB, LSB, CW and AM."))); }
        tickRow(v, "e.on", tr("Passband edges on"), on, [this, low, high, shift](bool o) { sendPass(o, low, high, shift, tr("Passband edges on"), {"e.low", "e.high", "e.shift"}); }, has,
                tr("Off, the mode's own filter is all there is. On, the low cut, the high cut and the shift below are in force."));
        const std::string lowTip = tr("USB, LSB and CW: the lowest audio frequency kept, in Hz. AM: how far below the carrier the band reaches.");
        const std::string highTip = tr("USB, LSB and CW: the highest audio frequency kept, above the low cut. AM: how far above the carrier the band reaches.");
        field(v, "e.low", tr("Low cut (Hz)"), numberText(low), [this, mode, high, shift](const std::string& t) {
            double h = 0; std::string e;
            if (!sll::parseNumber(t, kCutHz.lo, kCutHz.hi, h, e) || !checkCuts(mode, h, high, e)) { refuseHere(kRadio, tr("Low cut (Hz)"), e); return; }
            sendPass(true, h, high, shift, tr("Low cut (Hz)"), {"e.low", "e.high", "e.shift"});
        }, lowTip);
        field(v, "e.high", tr("High cut (Hz)"), numberText(high), [this, mode, low, shift](const std::string& t) {
            double h = 0; std::string e;
            if (!sll::parseNumber(t, kCutHz.lo, kCutHz.hi, h, e) || !checkCuts(mode, low, h, e)) { refuseHere(kRadio, tr("High cut (Hz)"), e); return; }
            sendPass(true, low, h, shift, tr("High cut (Hz)"), {"e.low", "e.high", "e.shift"});
        }, highTip);
        field(v, "e.shift", tr("IF shift (Hz)"), numberText(shift), [this, low, high](const std::string& t) {
            double h = 0; std::string e;
            if (!sll::parseNumber(t, kShiftHz.lo, kShiftHz.hi, h, e)) { refuseHere(kRadio, tr("IF shift (Hz)"), e); return; }
            sendPass(true, low, high, h, tr("IF shift (Hz)"), {"e.low", "e.high", "e.shift"});
        }, tr("-6000 to +6000 Hz. A positive shift moves the band to higher audio frequencies, in USB and LSB alike."));
        key(v, "e.down", tr("Shift down 100 Hz"), [this, low, high, shift] { sendPass(true, low, high, std::max(kShiftHz.lo, shift - 100.0), tr("Shift down 100 Hz"), {"e.low", "e.high", "e.shift"}); }, has);
        key(v, "e.up", tr("Shift up 100 Hz"), [this, low, high, shift] { sendPass(true, low, high, std::min(kShiftHz.hi, shift + 100.0), tr("Shift up 100 Hz"), {"e.low", "e.high", "e.shift"}); }, has);
        v.push_back(rReadout(tr("Edges in the filter"), pf.ok && pf.active ? tr("yes") : tr("no")));
    }

    // ---- the AM detector (G1-19)
    void buildAmDetector(std::vector<Row>& v) {
        if (!fold(v, "am", tr("AM detector"), tr("How an AM signal is detected: by its envelope, as always, or by a loop that locks on the carrier."))) { return; }
        const AmDetFact af = parseAmDetFact(fact("radio." + rn() + ".amdet"));
        const int kind = af.ok ? af.kind : 0;
        if (st.demodMode != FOXAPI_DEMOD_AM) { v.push_back(rNote(tr("The detector acts in AM mode only."))); }
        choice(v, "d.kind", tr("Detection"), amDetectorNames(), kind, [this](int i) { send(kRadio, tr("Detection"), cmd(FOXAPP_OP_SET_AM_DETECTOR, i)); },
               tr("Synchronous detection follows the carrier, so a fading carrier or a neighbour on one side does not distort the sound; on one sideband it rejects the other. It falls back to the envelope detector by itself while there is no carrier."));
        v.push_back(rReadout(tr("Carrier lock"), kind == 0 ? tr("not in use") : (af.locked ? tr("locked") + ", " + signedHz(af.offsetHz) : tr("searching"))));
    }

    void sendTone(unsigned kind, double hz, int code, int pol, const std::string& what, const std::vector<std::string>& drops) {
        send(kRadio, what, cmd(FOXAPI_OP_SET_TONE_SQUELCH, kind, 1, hz, code, pol), drops);
    }

    void buildSquelch(std::vector<Row>& v) {
        if (!fold(v, "sql", tr("Noise and tone squelch"), tr("Squelches that open on the noise of an FM signal or on a CTCSS tone or DCS code, beside the level squelch of the receiver strip."))) { return; }
        if (st.demodMode != FOXAPI_DEMOD_NFM) { v.push_back(rNote(tr("These squelches work in NFM only."))); }
        const bool nOn = st.noiseSquelchOn != 0u;
        const double thr = st.noiseSquelchThresholdDb, tail = st.noiseSquelchTailMs;
        tickRow(v, "q.noise", tr("Noise squelch"), nOn, [this, thr, tail](bool o) { send(kRadio, tr("Noise squelch"), cmd(FOXAPI_OP_SET_NOISE_SQUELCH, o ? 1 : 0, 1, thr, tail)); }, true,
                tr("Opens when the noise above 5 kHz in the FM discriminator falls below the threshold, so a quiet channel stays shut whatever its level."));
        slider(v, "q.thr", tr("Noise threshold"), kNoiseSqThreshold, 1.0, thr, dbPlain, [this, nOn, tail](double d) { send(kRadio, tr("Noise threshold"), cmd(FOXAPI_OP_SET_NOISE_SQUELCH, nOn ? 1 : 0, 1, d, tail)); },
               tr("-90 to 0 dB re full scale: the gate opens when the noise falls below this."));
        slider(v, "q.tail", tr("Noise tail"), kNoiseSqTail, 50.0, tail, msText, [this, nOn, thr](double t) { send(kRadio, tr("Noise tail"), cmd(FOXAPI_OP_SET_NOISE_SQUELCH, nOn ? 1 : 0, 1, thr, t)); },
               tr("0 to 2000 ms: how long the noise must stay up before the gate closes."));
        v.push_back(rReadout(tr("Noise level now"), st.noiseSquelchLevelDb <= -199.0 ? tr("none") : dbOne(st.noiseSquelchLevelDb) + ", " + (st.noiseSquelchOpen != 0u ? tr("open") : tr("shut"))));

        const unsigned kind = st.toneKind;
        const double hz = st.toneHz;
        const int code = static_cast<int>(st.toneCode), pol = static_cast<int>(st.tonePolarity);
        choice(v, "q.kind", tr("Tone squelch"), toneKindNames(), static_cast<int>(kind), [this](int i) { send(kRadio, tr("Tone squelch"), cmd(FOXAPI_OP_SET_TONE_SQUELCH, i, 0)); },
               tr("Opens only for a carrier that carries the tone or code chosen below. A tone of 0 (or a code of 0) lets any tone (any code) through."));
        field(v, "q.ctcss", tr("CTCSS tone (Hz)"), hz > 0.0 ? ctcssText(hz) : "0", [this, kind, code, pol](const std::string& t) {
            double h = 0; std::string e;
            if (!parseCtcss(t, h, e)) { refuseHere(kRadio, tr("CTCSS tone (Hz)"), e); return; }
            sendTone(kind, h, code, pol, tr("CTCSS tone (Hz)"), {"q.ctcss"});
        }, tr("One of the 50 standard tones, like 88.5; 0 means any tone."));
        key(v, "q.ctcss-prev", tr("Previous CTCSS tone"), [this, kind, hz, code, pol] { sendTone(kind, nextCtcss(hz, -1), code, pol, tr("Previous CTCSS tone"), {"q.ctcss"}); });
        key(v, "q.ctcss-next", tr("Next CTCSS tone"), [this, kind, hz, code, pol] { sendTone(kind, nextCtcss(hz, 1), code, pol, tr("Next CTCSS tone"), {"q.ctcss"}); });
        field(v, "q.dcs", tr("DCS code"), code > 0 ? dcsText(code) : "0", [this, kind, hz, pol](const std::string& t) {
            int c = 0; std::string e;
            if (!parseDcs(t, c, e)) { refuseHere(kRadio, tr("DCS code"), e); return; }
            sendTone(kind, hz, c, pol, tr("DCS code"), {"q.dcs"});
        }, tr("One of the 83 standard codes, written like 023; 0 means any code."));
        key(v, "q.dcs-prev", tr("Previous DCS code"), [this, kind, hz, code, pol] { sendTone(kind, hz, nextDcs(code, -1), pol, tr("Previous DCS code"), {"q.dcs"}); });
        key(v, "q.dcs-next", tr("Next DCS code"), [this, kind, hz, code, pol] { sendTone(kind, hz, nextDcs(code, 1), pol, tr("Next DCS code"), {"q.dcs"}); });
        choice(v, "q.pol", tr("DCS polarity"), polarityNames(), pol, [this, kind, hz, code](int i) { sendTone(kind, hz, code, i, tr("DCS polarity"), {}); },
               tr("Some repeaters send a DCS code inverted."));
        tickRow(v, "q.remove", tr("Remove the tone from the audio"), st.toneRemove != 0u, [this](bool o) { send(kRadio, tr("Remove the tone from the audio"), cmd(FOXAPI_OP_SET_TONE_REMOVE, o ? 1 : 0)); }, true,
                tr("A high-pass that takes the sub-audible tone out of what you hear."));
        std::string heard = tr("none");
        if (st.toneDetectedHz > 0.0) { heard = ctcssText(st.toneDetectedHz) + " Hz"; }
        else if (st.toneDetectedCode > 0u) { heard = "DCS " + dcsText(static_cast<int>(st.toneDetectedCode)); }
        v.push_back(rReadout(tr("Tone heard"), heard));
        v.push_back(rReadout(tr("Tone gate"), st.toneGate != 0u ? tr("open") : tr("shut")));
    }

    void buildTracking(std::vector<Row>& v) {
        if (!fold(v, "afc", tr("Frequency tracking and multiplex"), tr("Follows a carrier that drifts (AFC, or auto-tune in CW), and records the FM multiplex for a program such as RDS Spy."))) { return; }
        const bool cw = st.demodMode == FOXAPI_DEMOD_CW;
        const bool on = st.afcOn != 0u;
        const std::string afcLabel = cw ? tr("CW auto-tune") : tr("Automatic frequency control (AFC)");
        tickRow(v, "a.afc", afcLabel, on, [this, afcLabel](bool o) { send(kRadio, afcLabel, cmd(FOXAPI_OP_SET_AFC, o ? 1 : 0, 0)); }, true,
                tr("Keeps the dial on a carrier that drifts. It works in NFM, WFM, AM and CW (in CW it finds the carrier's peak); in the other modes it does nothing."));
        field(v, "a.pull", tr("AFC pull-in range (Hz)"), numberText(st.afcPullInHz), [this, on](const std::string& t) {
            double h = 0; std::string e;
            if (!sll::parseNumber(t, kAfcPullIn.lo, kAfcPullIn.hi, h, e)) { refuseHere(kRadio, tr("AFC pull-in range (Hz)"), e); return; }
            send(kRadio, tr("AFC pull-in range (Hz)"), cmd(FOXAPI_OP_SET_AFC, on ? 1 : 0, 1, h), {"a.pull"});
        }, tr("50 to 20000 Hz: how far from the dial a carrier may be and still be caught."));
        v.push_back(rReadout(tr("AFC following"), on ? signedHz(st.afcCorrectionHz) + ", " + (st.afcLocked != 0u ? tr("locked") : tr("not locked")) : tr("off")));
        const bool wfm = st.demodMode == FOXAPI_DEMOD_WFM;
        const bool rec = fact("radio." + rn() + ".mpx") == "1";
        tickRow(v, "a.mpx", tr("Record the FM multiplex"), rec, [this](bool o) { send(kRadio, tr("Record the FM multiplex"), cmd(FOXAPP_OP_MPX_RECORD, o ? 1 : 0)); }, wfm || rec,
                tr("Writes the whole FM signal before it is split into sound and data, at 192 kHz, to a WAV file in the recordings folder, for a program such as RDS Spy. Only in WFM."));
    }

    void buildRadio(std::vector<Row>& v) {
        if (!haveState) { return; }
        buildMeter(v);
        buildCw(v);
        buildPassband(v);
        buildEdges(v);
        buildAmDetector(v);
        buildSquelch(v);
        buildTracking(v);
        endNote(v, kRadio);
    }

    // ================================================================ SIGNAL > Audio filters
    void buildEq(std::vector<Row>& v) {
        if (!fold(v, "eq", tr("Audio equaliser"), tr("Eight bands on the finished audio: bass, middle and treble, and five graphic bands."))) { return; }
        const std::vector<std::string> names = eqBandNames();
        for (int i = 0; i < kEqBands; ++i) {
            const std::string label = tr("EQ") + " " + names[static_cast<std::size_t>(i)];
            slider(v, "e." + std::to_string(i), label, kEqDb, 0.5, st.eqDb[i], dbText, [this, i, label](double db) { send(kAudio, label, cmd(FOXAPI_OP_SET_EQ_BAND, i, 0, db)); }, tr("-12 to +12 dB; 0 is flat."));
        }
        key(v, "e.flat", tr("Flatten the equaliser"), [this] {
            int n = 0;
            for (int i = 0; i < kEqBands; ++i) { if (st.eqDb[i] != 0.0) { send(kAudio, tr("Flatten the equaliser"), cmd(FOXAPI_OP_SET_EQ_BAND, i, 0, 0.0)); ++n; held.erase("e." + std::to_string(i)); } }
            if (n == 0) { setNote(kAudio, tr("Flatten the equaliser") + ": " + tr("it is flat already"), kDim); }
        });
    }

    void buildPass(std::vector<Row>& v) {
        if (!fold(v, "hplp", tr("Audio high-pass and low-pass"), tr("Cuts the lowest or the highest of the sound you hear."))) { return; }
        const int hp = indexOfHighPass(st.audioHighPassHz), lp = indexOfLowPass(st.audioLowPassHz);
        choice(v, "f.hp", tr("Audio high-pass"), highPassNames(), hp, [this, lp](int i) {
            send(kAudio, tr("Audio high-pass"), cmd(FOXAPI_OP_SET_AUDIO_FILTERS, kHighPassHz[i], lp >= 0 ? kLowPassHz[lp] : 0));
        }, tr("Takes out rumble and hum below 100 or 300 Hz."));
        choice(v, "f.lp", tr("Audio low-pass"), lowPassNames(), lp, [this, hp](int i) {
            send(kAudio, tr("Audio low-pass"), cmd(FOXAPI_OP_SET_AUDIO_FILTERS, hp >= 0 ? kHighPassHz[hp] : 0, kLowPassHz[i]));
        }, tr("Takes out hiss above 2.4, 3 or 6 kHz."));
    }

    void buildNr(std::vector<Row>& v) {
        if (!fold(v, "nrm", tr("Noise-reduction method"), tr("How the noise reduction above works; its strength is the slider there."))) { return; }
        choice(v, "r.method", tr("Reduction method"), nrMethodNames(), static_cast<int>(st.nrMethod), [this](int i) { send(kAudio, tr("Reduction method"), cmd(FOXAPI_OP_SET_NR_METHOD, i)); },
               tr("Standard is the original. Spectral takes steady hiss out of voice. The line enhancer pulls a steady tone out of noise and works in CW and RAW only; elsewhere the standard one runs."));
        const auto names = nrMethodNames();
        const unsigned active = st.nrMethodActive;
        v.push_back(rReadout(tr("In use now"), active < names.size() ? names[active] : numberText(active)));
    }

    void buildNotches(std::vector<Row>& v) {
        if (!fold(v, "notch", tr("Notch bank"), tr("Up to ten manual notches on the audio, each at its own frequency and width, beside the single notch and the auto notch above."))) { return; }
        const std::string summary = notchSummary(st.notchBankMask, st.notchBankHz, st.notchBankWidthHz);
        v.push_back(rReadout(tr("Notches on"), summary.empty() ? tr("none") : summary));
        int slot0 = 0;
        std::string e;
        const bool slotOk = parseNotchSlot(dv("n.slot", "1"), slot0, e);
        if (!slotOk) { slot0 = 0; }
        typed(v, "n.slot", tr("Notch slot (1 to 10)"), "1", tr("Which of the ten notches the rows below are about."));
        handlers["n.slot"] = [this](const std::string& t) { draft["n.slot"] = t; draft.erase("n.hz"); draft.erase("n.width"); };   // another slot: its own figures, not what was typed for the last
        if (!slotOk) { v.push_back(rNote(e, kErr)); }
        const bool slotOn = (st.notchBankMask & (1u << slot0)) != 0u;
        tickRow(v, "n.on", tr("Notch in this slot"), slotOn, [this, slot0](bool o) { send(kAudio, tr("Notch in this slot"), cmd(FOXAPI_OP_SET_NOTCH_SLOT, o ? 1 : 0, 0, 0, 0, slot0)); }, slotOk,
                tr("Switches this slot's notch on or off at the frequency and width it has."));
        typed(v, "n.hz", tr("Notch frequency (Hz)"), numberText(st.notchBankHz[slot0]), tr("20 to 21600 Hz"));
        typed(v, "n.width", tr("Notch width (Hz)"), numberText(st.notchBankWidthHz[slot0]), tr("1 to 2000 Hz, at the notch's -3 dB points"));
        key(v, "n.set", tr("Set this notch"), [this, slot0, slotOk] {
            if (!slotOk) { return; }
            double hz = 0, w = 0; std::string er;
            if (!parseNotchHz(dv("n.hz", numberText(st.notchBankHz[slot0])), hz, er)) { refuseHere(kAudio, tr("Notch frequency (Hz)"), er); return; }
            if (!parseNotchWidth(dv("n.width", numberText(st.notchBankWidthHz[slot0])), w, er)) { refuseHere(kAudio, tr("Notch width (Hz)"), er); return; }
            send(kAudio, tr("Set this notch"), cmd(FOXAPI_OP_SET_NOTCH_SLOT, 1, 1, hz, w, slot0), {"n.hz", "n.width"});
        }, slotOk, tr("Puts a notch in this slot at the frequency and width typed above, and switches it on."));
        key(v, "n.clear", tr("Clear every notch"), [this] {
            int n = 0;
            for (int i = 0; i < kNotchSlots; ++i) { if ((st.notchBankMask & (1u << i)) != 0u) { send(kAudio, tr("Clear every notch"), cmd(FOXAPI_OP_SET_NOTCH_SLOT, 0, 0, 0, 0, i)); ++n; } }
            if (n == 0) { setNote(kAudio, tr("Clear every notch") + ": " + tr("none is on"), kDim); }
        });
    }

    void buildPlacement(std::vector<Row>& v) {
        if (!fold(v, "place", tr("Stereo placement"), tr("Places the sound between your ears: a pan, a pseudo-stereo spread, or a binaural angle."))) { return; }
        const int mode = static_cast<int>(st.stereoPlaceMode);
        choice(v, "s.mode", tr("Placement mode"), placementNames(), mode, [this](int i) { send(kAudio, tr("Placement mode"), cmd(FOXAPI_OP_SET_STEREO_PLACE, i, 0, i == 0 ? 0.0 : placementDefault(i))); },
               tr("Pan puts the sound left or right. Pseudo-stereo spreads a mono sound into two a little different. Binaural places it at an angle with a time and level difference between the ears."));
        if (mode >= 1 && mode <= 3) {
            const Range r = placementRange(mode);
            double shown = st.stereoPlaceValue;
            const std::string id = "s.value";
            const auto h = held.find(id);
            if (h != held.end() && tickNo - h->second.tick < 15) { shown = h->second.value; }
            v.push_back(rSlider(id, tr("Placement"), toLevel(r, shown), placementText(mode, shown), true,
                                mode == 1 ? tr("Left to right.") : mode == 2 ? tr("0 to 100 percent.") : tr("-90 to +90 degrees.")));
            const double step = mode == 3 ? 1.0 : 0.05;
            handlers[id] = [this, id, r, step, mode](const std::string& t) {
                const double value = fromLevelStep(r, std::atof(t.c_str()), step);
                const auto h2 = held.find(id);
                if (h2 != held.end() && tickNo - h2->second.tick < 15 && h2->second.value == value) { return; }
                held[id] = {value, tickNo};
                send(kAudio, tr("Placement"), cmd(FOXAPI_OP_SET_STEREO_PLACE, mode, 0, value));
            };
        }
    }

    void buildAudio(std::vector<Row>& v) {
        if (!haveState) { return; }
        buildNr(v);
        buildNotches(v);
        buildPass(v);
        buildEq(v);
        buildPlacement(v);
        endNote(v, kAudio);
    }

    // ================================================================ the models
    std::array<std::shared_ptr<slint::VectorModel<KRow>>, kSecCount> models;
    std::array<std::vector<std::string>, kSecCount> sigs;
    std::array<std::vector<std::string>, kSecCount> shapes;

    void publishSection(Sec s, const std::vector<Row>& rows) {
        std::vector<std::string> sg, sh;
        sg.reserve(rows.size());
        sh.reserve(rows.size());
        for (const auto& r : rows) { sg.push_back(rowSig(r)); sh.push_back(std::to_string(r.kind) + r.id); }
        lastCount[s] = std::to_string(rows.size());
        if (models[s] && sh == shapes[s]) {
            // the same list of controls: only the rows whose content moved are set again, so a slider under a pointer is not rebuilt
            for (std::size_t i = 0; i < rows.size(); ++i) { if (sg[i] != sigs[s][i]) { models[s]->set_row_data(i, toK(rows[i])); } }
            sigs[s] = std::move(sg);
            return;
        }
        sigs[s] = std::move(sg);
        shapes[s] = std::move(sh);
        std::vector<KRow> ks;
        ks.reserve(rows.size());
        for (const auto& r : rows) { ks.push_back(toK(r)); }
        models[s] = std::make_shared<slint::VectorModel<KRow>>(std::move(ks));
        if (env.ui == nullptr) { return; }
        auto& l = env.ui->global<LaneS1>();
        switch (s) {
            case kSource: l.set_rows_source(models[s]); break;
            case kRadio: l.set_rows_radio(models[s]); break;
            case kAudio: l.set_rows_audio(models[s]); break;
            default: break;
        }
    }

    void rebuildAll() {
        dirty = false;
        using Builder = void (Impl::*)(std::vector<Row>&);
        static const Builder builders[kSecCount] = {&Impl::buildSource, &Impl::buildRadio, &Impl::buildAudio};
        handlers.clear();
        for (int s = 0; s < kSecCount; ++s) {
            std::vector<Row> rows;
            (this->*builders[s])(rows);
            publishSection(static_cast<Sec>(s), rows);
        }
    }

    void act(const std::string& id, const std::string& text) {
        const auto it = handlers.find(id);
        if (it == handlers.end()) { return; }
        const Handler h = it->second;
        h(text);
        dirty = true;
        rebuildAll();
    }
};

S1Publisher::S1Publisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
S1Publisher::~S1Publisher() = default;

void S1Publisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneS1>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void S1Publisher::tick(const FoxReceiverState& state, long tickNo) {
    Impl& p = *p_;
    p.tickNo = tickNo;
    p.st = state;
    p.haveState = true;
    const int sel = p.sel();
    if (sel != p.lastSel) {   // another radio is selected: what was typed for the last one is not for this one (the groups stay as they were)
        for (auto it = p.draft.begin(); it != p.draft.end();) { it = it->first.rfind("g.", 0) == 0 ? std::next(it) : p.draft.erase(it); }
        p.held.clear();
        p.lastSel = sel;
        p.dirty = true;
    }
    // the rows are read again from the engine's state about six times a second, and at once after any action or answer
    if (p.dirty || tickNo - p.lastRebuildTick >= 5) {
        p.lastRebuildTick = tickNo;
        p.rebuildAll();
    }
}

void S1Publisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

std::string S1Publisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "s1.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "s1.rows.<section>, not " + sec;
    }
    if (what.rfind("draft.", 0) == 0) { out = p.dv(what.substr(6)); return std::string(); }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    if (what.rfind("state.", 0) == 0) {
        double v = 0.0;
        if (!stateValue(p.st, what.substr(6), v)) { return "s1.state.<an engine field of the selected radio>, not " + what; }
        char b[64];
        std::snprintf(b, sizeof(b), "%.6g", v);
        out = b;
        return std::string();
    }
    return "s1.<sent|refused|answered|note.<section>|rows.<section>|draft.<id>|fact.<name>|state.<field>>, not " + what;
}

}  // namespace foxsdr::slints1
