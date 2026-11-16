// lanes2_publish.cpp - see lanes2_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanes2_publish.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/health_events.hpp"
#include "core/i18n.hpp"
#include "core/operating_aids.hpp"
#include "core/power_log.hpp"
#include "core/utf8_text.hpp"
#include "host_logic.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "lanes2_logic.hpp"
#include "plugin_kit.hpp"

namespace foxsdr::slints2 {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
namespace oa = cascade::core::aids;
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
using slk::rTick;
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

enum Sec { kSpectrum = 0, kZoom, kLog, kTuning, kVfos, kSecCount };
const char* const kSecName[kSecCount] = {"spectrum", "zoom", "log", "tuning", "vfos"};

struct Cmd {
    std::uint32_t op = 0;
    long long i0 = 0, i1 = 0;
    double n0 = 0, n1 = 0, n2 = 0;
    std::string text;
};
Cmd cmd(std::uint32_t op, long long i0 = 0) { Cmd c; c.op = op; c.i0 = i0; return c; }

// the window names as the string tables know them (windowName() gives the English the logic keeps)
const char* const kWindowWords[kWindowCount] = {FOX_TR_NOOP("Rectangular"), FOX_TR_NOOP("Hann"), FOX_TR_NOOP("Hamming"), FOX_TR_NOOP("Blackman-Harris"), FOX_TR_NOOP("Flat top")};

std::uint32_t demodOfName(const std::string& name) {
    for (std::uint32_t d = 1; d <= 8; ++d) { if (name == foxsdr::slinthost::modeName(d)) { return d; } }
    return 0;
}
std::string fmtHz(double v, const char* f) { std::string s; cascade::core::formatUtf8(s, f, v); return s; }
}  // namespace

struct LaneS2Publisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    std::map<std::string, std::string> draft;     // what the person typed or chose and has not sent (never saved)
    struct Pending { int sec = 0; std::string what; std::string draftToDrop; };
    std::map<std::uint64_t, Pending> pending;     // ticket -> section, what it was
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> sig;
    std::array<std::string, kSecCount> lastCount;
    std::array<std::shared_ptr<slint::VectorModel<KRow>>, kSecCount> models;
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    bool logFailTold = false;                     // the log's disk failure has been counted for the owner (once for each run of the log)
    long lastRebuildTick = -100, tickNow = 0;
    FoxReceiverState st{};                        // the selected radio's state, copied every tick
    bool haveState = false;
    std::string stateSig;

    // ---- autosave of the waterfall
    bool autoOn = false;
    int autoMinutes = 5;
    std::string autoFolder;
    double autoLastMs = 0.0;
    bool autoWaiting = false;                     // a picture was handed to the host's writer and has not come back
    std::string autoNote;
    int autoNoteTone = 0;
    long autoSaved = 0;
    std::uint64_t autoCounter = 0;
    std::string autoLastPath;

    // ---- the power and SNR log
    std::unique_ptr<cascade::core::powerlog::CsvLogger> logger;
    std::string logFolder;
    double logIntervalS = 1.0;
    std::string logNote;
    int logNoteTone = 0;

    // ---- zoom picture
    std::vector<float> zoomBins;
    std::string zoomCaption;
    long zoomPictures = 0;

    // ---- band stacking
    oa::BandStacking stack;
    long stackIgnoreUntil = -1;                   // ticks: the live tune is not tracked while a recalled one is arriving
    std::string stackFile;
    std::string stackNote;
    int stackNoteTone = 0;

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {
        std::vector<oa::BandDef> defs;
        for (int i = 0; i < bandCount(); ++i) {
            const BandInfo& b = bandAt(i);
            oa::BandDef d;
            d.name = b.name; d.lowHz = b.lowHz; d.highHz = b.highHz;
            const double span = b.highHz - b.lowHz;
            const double at[3] = {b.centreHz, b.lowHz + span * 0.25, b.lowHz + span * 0.75};
            for (int k = 0; k < 3; ++k) { d.defaults[static_cast<std::size_t>(k)].hz = std::floor(at[k] / 1000.0) * 1000.0; d.defaults[static_cast<std::size_t>(k)].mode = b.mode; d.defaults[static_cast<std::size_t>(k)].bandwidthHz = b.bandwidthHz; }
            defs.push_back(d);
        }
        std::string err;
        (void)stack.setBands(defs, &err);
        if (!env.configDir.empty()) {
            stackFile = (std::filesystem::path(env.configDir) / "bandstack.json").string();
            std::ifstream f(stackFile, std::ios::binary);
            if (f) {
                std::stringstream ss2; ss2 << f.rdbuf();
                const nlohmann::json j = nlohmann::json::parse(ss2.str(), nullptr, false);
                if (!j.is_discarded() && !stack.fromJson(j)) { stackNote = tr("The saved band registers could not be read, so the bands start from their own values."); stackNoteTone = kWarn; }
            }
        }
    }

    // ---------------------------------------------------------------- facts and drafts
    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    int sel() { const int s = env.selectedRadio ? env.selectedRadio() : 1; return s < 1 ? 1 : s; }
    std::string rn() { return std::to_string(sel()); }
    std::string dv(const std::string& id, const std::string& def = std::string()) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second; }
    int pv(const std::string& id, int def) const { const auto it = draft.find(id); return it == draft.end() ? def : std::atoi(it->second.c_str()); }
    bool bv(const std::string& id, bool def) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second == "1"; }

    // ---------------------------------------------------------------- the one place a command leaves
    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    void send(Sec s, const std::string& what, const Cmd& k, const std::string& draftToDrop = std::string()) {
        if (!env.submit) { return; }
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = k.op;
        c.ival[0] = k.i0; c.ival[1] = k.i1;
        c.num[0] = k.n0; c.num[1] = k.n1; c.num[2] = k.n2;
        const std::string* longp = nullptr;
        if (k.text.size() < sizeof(c.text)) { std::memcpy(c.text, k.text.data(), k.text.size()); } else { longp = &k.text; }
        const SubmitOutcome o = env.submit(c, 0, longp);
        ++sent;
        if (o.ticket == 0 || sll::statusIsRefusal(o.status)) {
            ++refused;
            setNote(s, what + ": " + sll::refusalText(o.status, o.message), kErr);
        } else {
            pending[o.ticket] = {static_cast<int>(s), what, draftToDrop};
            setNote(s, what + ": " + tr("sent"), kDim);
        }
    }
    void refuseHere(Sec s, const std::string& what, const std::string& why) { ++refused; setNote(s, what + ": " + why, kErr); }

    void onAnswer(const FoxCommandResult& r) {
        const auto it = pending.find(r.ticket);
        if (it == pending.end()) { return; }
        const Sec s = static_cast<Sec>(it->second.sec);
        const std::string what = it->second.what;
        const std::string drop = it->second.draftToDrop;
        pending.erase(it);
        ++answered;
        const std::string msg = trim(r.message);
        if (r.status == FOXAPI_OK) {
            if (!drop.empty()) { draft.erase(drop); }
            setNote(s, what + ": " + tr("done") + (msg.empty() ? std::string() : " (" + msg + ")"), kGood);
        } else if (r.status == FOXAPI_NO_CHANGE) { setNote(s, what + ": " + sll::refusalText(r.status, msg), kDim); }
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
    void endNote(std::vector<Row>& v, Sec s) { if (!noteText[s].empty()) { v.push_back(rNote(noteText[s], noteTone[s])); } }

    // ================================================================ Spectrum tools: the FFT size, window and frame rate (G2-01, G2-02, G2-03)
    SpectrumSetting current() const {
        SpectrumSetting s;
        if (haveState) { s.size = st.spectrumFftSize != 0 ? st.spectrumFftSize : 1024u; s.window = static_cast<int>(st.spectrumWindow); s.fps = st.spectrumFrameRateHz; }
        return s;
    }
    void sendSpectrum(const std::string& what, const SpectrumSetting& s, const std::string& drop = std::string()) {
        Cmd c = cmd(FOXAPI_OP_SET_SPECTRUM, static_cast<long long>(s.size));
        c.i1 = s.window; c.n0 = s.fps;
        send(kSpectrum, what, c, drop);
    }
    void buildSpectrum(std::vector<Row>& v) {
        const SpectrumSetting cur = current();
        v.push_back(rHeading(tr("Spectrum detail and speed")));
        v.push_back(rReadout(tr("Spectrum now"), std::to_string(cur.size) + " " + tr("points") + ", " + tr(kWindowWords[std::clamp(cur.window, 0, kWindowCount - 1)]) + ", " + (cur.fps <= 0.0 ? tr("automatic rate") : sll::numberText(cur.fps) + " " + tr("a second"))));
        choice(v, "s.fft", tr("Spectrum points (FFT size)"), fftSizeLabels(), fftIndexOf(cur.size), [this](int i) {
            SpectrumSetting s = current(); s.size = fftSizeAt(i); sendSpectrum(tr("Spectrum points"), s);
        }, tr("More points show finer detail and cost more work for every frame. 1024 is the standard. A larger size makes the frames bigger."));
        std::vector<std::string> wn;
        for (int i = 0; i < kWindowCount; ++i) { wn.push_back(tr(kWindowWords[i])); }
        choice(v, "s.window", tr("Spectrum window"), wn, std::clamp(cur.window, 0, kWindowCount - 1), [this](int i) {
            SpectrumSetting s = current(); s.window = i; sendSpectrum(tr("Spectrum window"), s);
        }, tr("The shape the signal is cut with before the transform. Blackman-Harris is the standard: it keeps strong signals from smearing. Rectangular is sharpest but leaks the most. Flat top reads levels most exactly."));
        field(v, "s.fps", tr("Frame rate (per second)"), frameRateText(cur.fps), tr("0 or the word automatic = the engine chooses (about 30 a second); 1 to 120. Press Enter to apply."));
        handlers["s.fps"] = [this](const std::string& t) {
            draft["s.fps"] = t;
            double hz = 0; std::string e;
            if (!parseFrameRate(t, hz, e)) { refuseHere(kSpectrum, tr("Frame rate"), e); return; }
            SpectrumSetting s = current(); s.fps = hz; sendSpectrum(tr("Frame rate"), s, "s.fps");
        };
        endNote(v, kSpectrum);
    }

    // ================================================================ Spectrum tools: the IF zoom (G2-12)
    void buildZoom(std::vector<Row>& v) {
        const std::string zf = fact("radio." + rn() + ".zoom");
        ZoomFact z;
        (void)parseZoomFact(zf, z);
        v.push_back(rHeading(tr("Zoom on the passband (IF zoom)")));
        v.push_back(rNote(tr("A second, finer spectrum of just the channel you are listening to, drawn under the controls. It does not change what you hear. It is a view: it is off at every start.")));
        v.push_back(rReadout(tr("Zoom now"), z.on ? std::to_string(z.bins) + " " + tr("points") + ", " + fmtHz(z.binHz, "%.2f") + " Hz " + tr("a point") + ", " + fmtHz(z.spanHz, "%.0f") + " Hz" : tr("off")));
        const int pick = z.on ? zoomIndexOf(z.bins) : 0;
        choice(v, "z.size", tr("Zoom points"), zoomSizeLabels(tr("Off")), pick < 0 ? 0 : pick, [this, z](int i) {
            const std::uint32_t size = zoomSizeAt(i);
            Cmd c = cmd(FOXAPP_OP_SET_IF_ZOOM, static_cast<long long>(size));
            if (size != 0) {
                double span = 0; std::string e;
                if (!parseZoomSpan(dv("z.span", z.on ? fmtHz(z.spanHz, "%.0f") : "10000"), span, e)) { refuseHere(kZoom, tr("Zoom"), e); return; }
                c.n0 = span;
            }
            send(kZoom, size == 0 ? tr("Zoom off") : tr("Zoom"), c);
        }, tr("The number of points of the zoom spectrum: a power of two from 256 to 16384, or off."));
        field(v, "z.span", tr("Zoom width (Hz)"), z.on ? fmtHz(z.spanHz, "%.0f") : "10000", tr("The width of the whole zoom picture, 100 Hz up to the channel's own width; the engine shows the width it could give. Press Enter to apply when the zoom is on."));
        handlers["z.span"] = [this, z](const std::string& t) {
            draft["z.span"] = t;
            double span = 0; std::string e;
            if (!parseZoomSpan(t, span, e)) { refuseHere(kZoom, tr("Zoom width"), e); return; }
            if (!z.on) { setNote(kZoom, tr("Zoom width") + ": " + tr("kept for when you choose the zoom points"), kDim); return; }
            Cmd c = cmd(FOXAPP_OP_SET_IF_ZOOM, static_cast<long long>(z.bins)); c.n0 = span;
            send(kZoom, tr("Zoom width"), c, "z.span");
        };
        endNote(v, kZoom);
    }

    // ================================================================ Spectrum tools: waterfall autosave (G2-09) and the power and SNR log (G2-16)
    void buildLog(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Save the waterfall by itself")));
        v.push_back(rNote(tr("Writes a picture of the waterfall of the selected radio into the folder you choose, every so often, named by the recording file name rule. Nothing is written until you switch it on. It is off at every start.")));
        field(v, "a.folder", tr("Picture folder"), autoFolder, tr("The full path of the folder, for example C:/Users/you/Pictures/FoxSDR. It is made if it is not there."));
        handlers["a.folder"] = [this](const std::string& t) {
            draft["a.folder"] = t;
            const std::string f = cleanFolder(t);
            if (f.empty() && !trim(t).empty()) { autoNote = tr("Type the folder's full path, for example C:/Users/you/Pictures"); autoNoteTone = kErr; return; }
            autoFolder = f; autoNote.clear();
        };
        key(v, "a.choose", tr("Choose the picture folder..."), [this] {
            const std::string f = env.chooseFolder ? env.chooseFolder() : std::string();
            if (f.empty()) { return; }
            autoFolder = cleanFolder(f); draft["a.folder"] = autoFolder; autoNote.clear();
        }, true, tr("Opens the folder chooser."));
        std::vector<std::string> mins;
        for (int i = 0; i < kAutosaveChoices; ++i) { mins.push_back(std::to_string(autosaveMinutes(i)) + " " + tr("min")); }
        choice(v, "a.every", tr("Save a picture every"), mins, std::max(0, autosaveIndexOf(autoMinutes)), [this](int i) { autoMinutes = autosaveMinutes(i); });
        tick(v, "a.on", tr("Save the waterfall by itself"), autoOn, [this](bool on) {
            if (on) {
                const std::string f = cleanFolder(dv("a.folder", autoFolder));
                if (f.empty()) { autoNote = tr("Choose or type the folder first"); autoNoteTone = kErr; autoOn = false; return; }
                autoFolder = f; autoOn = true; autoLastMs = 0.0; autoNote = tr("On: the first picture is written at once"); autoNoteTone = kGood;
            } else { autoOn = false; autoNote = tr("Off"); autoNoteTone = kDim; }
        });
        if (autoSaved > 0) { v.push_back(rReadout(tr("Pictures saved"), std::to_string(autoSaved))); }
        if (!autoLastPath.empty()) { v.push_back(rReadout(tr("Last picture"), autoLastPath)); }
        if (!autoNote.empty()) { v.push_back(rNote(autoNote, autoNoteTone)); }

        v.push_back(rHeading(tr("Power and SNR log")));
        v.push_back(rNote(tr("Writes one line a moment for every radio window: the time, the radio, the tuned frequency, the bandwidth, the channel power, the noise floor and the SNR, to a CSV file in the folder you choose (one file a day). Nothing is written until you switch it on. It is off at every start.")));
        field(v, "l.folder", tr("Log folder"), logFolder, tr("The full path of the folder. The files are named foxsdr-power-<date>.csv."));
        handlers["l.folder"] = [this](const std::string& t) {
            draft["l.folder"] = t;
            const std::string f = cleanFolder(t);
            if (f.empty() && !trim(t).empty()) { logNote = tr("Type the folder's full path, for example C:/Users/you/Documents"); logNoteTone = kErr; return; }
            logFolder = f; logNote.clear();
        };
        key(v, "l.choose", tr("Choose the log folder..."), [this] {
            const std::string f = env.chooseFolder ? env.chooseFolder() : std::string();
            if (f.empty()) { return; }
            logFolder = cleanFolder(f); draft["l.folder"] = logFolder; logNote.clear();
        }, true, tr("Opens the folder chooser."));
        field(v, "l.interval", tr("Log a line every (s)"), sll::numberText(logIntervalS), tr("0.1 to 3600 seconds, for each radio"));
        handlers["l.interval"] = [this](const std::string& t) {
            draft["l.interval"] = t;
            double s = 0; std::string e;
            if (!parseLogInterval(t, s, e)) { logNote = e; logNoteTone = kErr; return; }
            logIntervalS = s; logNote.clear();
            if (logger) { stopLog(); startLog(); }   // a running log goes on with the new interval
        };
        tick(v, "l.on", tr("Log the power and the SNR"), logger != nullptr, [this](bool on) { if (on) { startLog(); } else { stopLog(); } });
        if (logger) {
            v.push_back(rReadout(tr("Lines written"), std::to_string(logger->written())));
            const std::string p = logger->currentPath();
            if (!p.empty()) { v.push_back(rReadout(tr("Log file"), p)); }
            if (!logger->diskHealthy()) { v.push_back(rNote(tr("The log file could not be written") + ": " + logger->lastError(), kErr)); }
        }
        if (!logNote.empty()) { v.push_back(rNote(logNote, logNoteTone)); }
        endNote(v, kLog);
    }
    void startLog() {
        const std::string f = cleanFolder(dv("l.folder", logFolder));
        if (f.empty()) { logNote = tr("Choose or type the folder first"); logNoteTone = kErr; return; }
        logFolder = f;
        cascade::core::powerlog::CsvOptions o;
        o.directory = f; o.intervalS = logIntervalS;
        logFailTold = false;
        logger = std::make_unique<cascade::core::powerlog::CsvLogger>(o);
        logger->start();
        logNote = tr("On"); logNoteTone = kGood;
    }
    void stopLog() {
        if (logger) { logger->stop(); logger.reset(); }
        logNote = tr("Off"); logNoteTone = kDim;
    }

    // ================================================================ Tuning: centre tuning (G2-31), VFO A / B (G2-33), band stacking (G2-32)
    void buildTuning(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Centre tuning")));
        const bool ctun = haveState && st.ctun != 0;
        tick(v, "t.ctun", tr("Centre tuning"), ctun, [this](bool on) { send(kTuning, tr("Centre tuning"), cmd(FOXAPI_OP_SET_CTUN, on ? 1 : 0)); },
             tr("On: a tune inside the span on show moves the VFO and leaves the radio's centre where it is, so the picture does not slide. Off: the centre follows the tune."));

        v.push_back(rHeading(tr("VFO A and B")));
        if (haveState) {
            const bool aActive = st.vfoActive == 0;
            const std::string live = vfoText(st.tunedHz, foxsdr::slinthost::modeName(st.demodMode), st.bandwidthHz);
            const std::string other = st.vfoOtherHz > 0.0 ? vfoText(st.vfoOtherHz, foxsdr::slinthost::modeName(st.vfoOtherMode), st.vfoOtherBandwidthHz) : tr("not set");
            v.push_back(rReadout(tr("VFO in use"), vfoLetter(st.vfoActive)));
            v.push_back(rReadout(tr("VFO A"), aActive ? live : other));
            v.push_back(rReadout(tr("VFO B"), aActive ? other : live));
        }
        key(v, "t.vfo-a", tr("Use VFO A"), [this] { send(kTuning, tr("Use VFO A"), cmd(FOXAPP_OP_VFO_SELECT, 0)); }, !haveState || st.vfoActive != 0);
        key(v, "t.vfo-b", tr("Use VFO B"), [this] { send(kTuning, tr("Use VFO B"), cmd(FOXAPP_OP_VFO_SELECT, 1)); }, !haveState || st.vfoActive != 1);
        key(v, "t.vfo-swap", tr("Swap the two VFOs"), [this] { send(kTuning, tr("Swap the two VFOs"), cmd(FOXAPP_OP_VFO_SWAP)); }, !haveState || st.vfoOtherHz > 0.0,
            tr("The radio takes the other VFO's frequency, mode and bandwidth, and the other keeps what the radio had."));
        key(v, "t.vfo-copy", tr("Copy this tune to the other VFO"), [this] { send(kTuning, tr("Copy this tune to the other VFO"), cmd(FOXAPP_OP_VFO_STORE, 0)); }, true,
            tr("Puts the frequency, mode and bandwidth you are on into the VFO you are not using."));
        v.push_back(rNote(tr("Receive only: both VFOs listen; neither one transmits.")));
        endNote(v, kTuning);

        v.push_back(rHeading(tr("Band stacking")));
        const double hz = haveState ? st.tunedHz : 0.0;
        const int cur = stack.bandOf(hz);
        v.push_back(rReadout(tr("Band"), cur >= 0 ? bandAt(cur).name : tr("none of the bands")));
        const bool open = bv("b.open", false);
        key(v, "b.more", open ? tr("Hide the band keys") : tr("Show the band keys"), [this, open] { draft["b.open"] = open ? "0" : "1"; });
        if (open) {
            std::vector<std::string> names;
            for (int i = 0; i < bandCount(); ++i) { names.push_back(bandAt(i).name); }
            choice(v, "b.pick", tr("Band"), names, cur, [this](int i) { pressBand(i); },
                   tr("Goes to where you last were in that band. Pressing the band you are in goes to the next of its three remembered places."));
        }
        if (cur >= 0) {
            for (int k = 0; k < oa::kRegistersPerBand; ++k) {
                const auto r = stack.read(cur, k);
                if (!r) { continue; }
                const bool active = stack.activeRegister(cur) == k;
                v.push_back(rReadout(tr("Place") + " " + std::to_string(k + 1) + (active ? " *" : ""), registerText(r->hz, r->mode, r->bandwidthHz)));
            }
            for (int k = 0; k < oa::kRegistersPerBand; ++k) {
                key(v, "b.store" + std::to_string(k), tr("Remember this tune as place") + " " + std::to_string(k + 1), [this, k] { storeRegister(k); });
            }
            key(v, "b.reset", tr("Put this band's places back"), [this, cur] { stack.reset(cur); saveStack(); setNote(kTuning, tr("Band places put back"), kGood); });
        }
        if (!stackNote.empty()) { v.push_back(rNote(stackNote, stackNoteTone)); }
    }

    void pressBand(int index) {
        const auto t = stack.pressBand(index);
        if (!t) { return; }
        saveStack();
        stackIgnoreUntil = tickNow + 45;
        const std::string what = tr("Band") + " " + bandAt(index).name;
        send(kTuning, what, [&] { Cmd c = cmd(FOXAPI_OP_SET_FREQUENCY); c.n0 = t->hz; return c; }());
        const std::uint32_t d = demodOfName(t->mode);
        if (d != 0) { send(kTuning, what, cmd(FOXAPI_OP_SET_MODE, static_cast<long long>(d))); }
        if (t->bandwidthHz > 0.0) { Cmd c = cmd(FOXAPI_OP_SET_BANDWIDTH); c.n0 = t->bandwidthHz; send(kTuning, what, c); }
    }
    void storeRegister(int k) {
        if (!haveState) { return; }
        const int b = stack.bandOf(st.tunedHz);
        if (b < 0) { return; }
        oa::TuneState s; s.hz = st.tunedHz; s.mode = foxsdr::slinthost::modeName(st.demodMode); s.bandwidthHz = st.bandwidthHz;
        if (stack.store(b, k, s)) { saveStack(); setNote(kTuning, tr("Remembered as place") + " " + std::to_string(k + 1), kGood); }
    }
    void saveStack() {
        if (stackFile.empty()) { return; }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(stackFile).parent_path(), ec);
        std::ofstream f(stackFile, std::ios::binary | std::ios::trunc);
        if (f) { f << stack.toJson().dump(1, ' ', false, nlohmann::json::error_handler_t::replace); }
        if (!f) { stackNote = tr("The band places could not be saved, so they are lost when the window closes."); stackNoteTone = kErr; } else { stackNote.clear(); }
    }
    // the live tune is what the current band's active register remembers
    void trackTune() {
        if (!haveState || tickNow < stackIgnoreUntil || st.tunedHz <= 0.0) { return; }
        oa::TuneState s; s.hz = st.tunedHz; s.mode = foxsdr::slinthost::modeName(st.demodMode); s.bandwidthHz = st.bandwidthHz;
        const int b = stack.bandOf(s.hz);
        if (b < 0) { return; }
        const auto r = stack.read(b, stack.activeRegister(b));
        if (r && r->hz == s.hz && r->mode == s.mode && r->bandwidthHz == s.bandwidthHz && stack.currentBand() == b) { return; }
        if (stack.track(s)) { stackTouched = tickNow; }
    }
    long stackTouched = -1000;

    // ================================================================ the several VFOs of the selected radio (G2-34)
    void buildVfos(std::vector<Row>&) {}

    // ================================================================ the models
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
        auto& l = env.ui->global<LaneS2>();
        switch (s) {
            case kSpectrum: l.set_rows_spectrum(models[s]); break;
            case kZoom: l.set_rows_zoom(models[s]); break;
            case kLog: l.set_rows_log(models[s]); break;
            case kTuning: l.set_rows_tuning(models[s]); break;
            case kVfos: l.set_rows_vfos(models[s]); break;
            default: break;
        }
    }

    void rebuildAll() {
        dirty = false;
        using Builder = void (Impl::*)(std::vector<Row>&);
        static const Builder builders[kSecCount] = {&Impl::buildSpectrum, &Impl::buildZoom, &Impl::buildLog, &Impl::buildTuning, &Impl::buildVfos};
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
        rebuildAll();
    }

    // ================================================================ per tick: the zoom picture, the autosave
    void paintZoomPicture() {
        if (env.ui == nullptr) { return; }
        auto& l = env.ui->global<LaneS2>();
        ZoomFact z;
        const bool on = parseZoomFact(fact("radio." + rn() + ".zoom"), z) && z.on;
        if (!on || !env.zoomFrame) { if (l.get_zoom_w() != 0) { l.set_zoom_w(0); } return; }
        double binHz = 0, spanHz = 0, usable = 0;
        zoomBins.assign(z.bins, 0.0f);
        if (!env.zoomFrame(static_cast<std::uint32_t>(sel()), zoomBins, binHz, spanHz, usable)) { return; }
        constexpr std::uint32_t W = 320, H = 96;
        const double lo = haveState ? st.dbMin : -120.0, hi = haveState ? st.dbMax : 0.0;
        const std::vector<std::uint8_t> px = paintZoom(W, H, zoomBins.data(), zoomBins.size(), lo, hi, spanHz > 0.0 ? usable / spanHz : 0.8);
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(W, H);
        std::memcpy(buf.begin(), px.data(), px.size());
        l.set_zoom_picture(slint::Image(buf));
        l.set_zoom_w(static_cast<int>(W)); l.set_zoom_h(static_cast<int>(H));
        zoomCaption = tr("Zoom spectrum of the passband") + ": " + fmtHz(spanHz, "%.0f") + " Hz";
        l.set_zoom_caption(ss(zoomCaption));
        ++zoomPictures;
    }

    void runAutosave() {
        if (autoWaiting && env.pollPicture) {
            PictureDone d;
            if (env.pollPicture(d)) {
                autoWaiting = false;
                if (d.ok) { ++autoSaved; autoLastPath = d.path; autoNote = tr("Saved") + " " + d.path; autoNoteTone = kGood; }
                else { autoNote = tr("The picture could not be written") + ": " + d.error; autoNoteTone = kErr; }
                dirty = true;
            }
        }
        if (!autoOn || autoWaiting || !env.startPicture || !env.steadyMs) { return; }
        const double now = env.steadyMs();
        if (!autosaveDue(static_cast<std::int64_t>(autoLastMs), static_cast<std::int64_t>(now), autoMinutes)) { return; }
        AutosaveFields f;
        f.unixMs = env.utcMs ? env.utcMs() : 0;
        f.frequencyHz = haveState ? st.tunedHz : 0.0;
        f.mode = haveState ? foxsdr::slinthost::modeName(st.demodMode) : std::string();
        f.radio = sel();
        const std::string path = autosavePath(autoFolder, fact("record.nameTemplate"), f, ++autoCounter);
        if (path.empty()) { autoNote = tr("The picture folder is not a usable folder"); autoNoteTone = kErr; autoOn = false; dirty = true; return; }
        const PictureStart r = env.startPicture(path);
        if (r == PictureStart::Started) { autoWaiting = true; autoLastMs = now > 0.0 ? now : 1.0; }
        else if (r == PictureStart::NoSpectrum) { autoNote = tr("This radio has shown no spectrum yet, so there is nothing to save"); autoNoteTone = kWarn; dirty = true; }
        // Busy: the host's writer is on another file; the next tick asks again
    }
};

LaneS2Publisher::LaneS2Publisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
LaneS2Publisher::~LaneS2Publisher() { p_->stopLog(); }

void LaneS2Publisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneS2>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void LaneS2Publisher::tick(const FoxReceiverState& s, long tickNo) {
    Impl& p = *p_;
    p.tickNow = tickNo;
    p.st = s;
    p.haveState = true;
    // what the rows read out of the state record: a change made elsewhere (a bookmark, Legacy, the web page) shows here at once
    char b[256];
    std::snprintf(b, sizeof b, "%u|%u|%.3f|%u|%u|%.1f|%.1f|%.1f|%u|%u|%.0f|%.0f", s.spectrumFftSize, s.spectrumWindow, s.spectrumFrameRateHz, s.ctun, s.vfoActive, s.vfoOtherHz, s.vfoOtherBandwidthHz, s.tunedHz, s.vfoOtherMode, s.demodMode, s.bandwidthHz, s.dbMin);
    const std::string sg = b;
    if (sg != p.stateSig) { p.stateSig = sg; p.dirty = true; }
    p.trackTune();
    if (p.dirty || tickNo - p.lastRebuildTick >= 15) {
        p.lastRebuildTick = tickNo;
        p.rebuildAll();
    }
    p.runAutosave();
    if (p.logger && !p.logger->diskHealthy() && !p.logFailTold) { p.logFailTold = true; cascade::core::health::noteRecordFailed(); p.dirty = true; }   // the log's file could not be written: counted for the owner, and the panel says so beside the key
    if (tickNo % 3 == 0) { p.paintZoomPicture(); }
}

void LaneS2Publisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

void LaneS2Publisher::frameSeen(int radio, const float* bins, std::size_t n, double centreHz, double spanHz, double tunedHz, double bandwidthHz) {
    Impl& p = *p_;
    if (!p.logger || bins == nullptr || n == 0 || !(spanHz > 0.0)) { return; }
    const double binHz = spanHz / static_cast<double>(n), lo = centreHz - spanHz / 2.0;
    const auto binOf = [&](double hz) { return static_cast<std::size_t>(std::clamp((hz - lo) / binHz, 0.0, static_cast<double>(n))); };
    const std::size_t p0 = binOf(tunedHz - bandwidthHz / 2.0), p1 = std::max(p0 + 1, binOf(tunedHz + bandwidthHz / 2.0));
    const PowerReading r = readPower(bins, n, p0, p1);
    if (!r.valid) { return; }
    cascade::core::powerlog::Record rec;
    rec.unixNs = (p.env.utcMs ? p.env.utcMs() : 0) * 1000000LL;
    rec.radio = radio;
    rec.frequencyHz = tunedHz;
    rec.bandwidthHz = bandwidthHz;
    rec.signalDbfs = r.signalDbfs;
    rec.noiseDbfs = r.noiseDbfs;
    rec.snrDb = r.snrDb;
    (void)p.logger->offer(rec);   // an interval or a full queue is counted by the logger itself and shown under the key
}

std::string LaneS2Publisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "s2.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "s2.rows.<section>, not " + sec;
    }
    if (what.rfind("draft.", 0) == 0) { out = p.dv(what.substr(6)); return std::string(); }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    // the engine's own state record, as the engine says it (the receiver state of the selected radio, read again every tick)
    if (what == "engine.fft") { out = std::to_string(p.st.spectrumFftSize); return std::string(); }
    if (what == "engine.window") { out = std::to_string(p.st.spectrumWindow); return std::string(); }
    if (what == "engine.fps") { char b[32]; std::snprintf(b, sizeof b, "%g", p.st.spectrumFrameRateHz); out = b; return std::string(); }
    if (what == "engine.ctun") { out = std::to_string(p.st.ctun); return std::string(); }
    if (what == "engine.vfo") { out = std::to_string(p.st.vfoActive); return std::string(); }
    if (what == "engine.vfoother") { char b[48]; std::snprintf(b, sizeof b, "%.0f", p.st.vfoOtherHz); out = b; return std::string(); }
    if (what == "engine.tuned") { char b[48]; std::snprintf(b, sizeof b, "%.0f", p.st.tunedHz); out = b; return std::string(); }
    if (what == "engine.mode") { out = foxsdr::slinthost::modeName(p.st.demodMode); return std::string(); }
    if (what == "autosave.on") { out = p.autoOn ? "1" : "0"; return std::string(); }
    if (what == "autosave.saved") { out = std::to_string(p.autoSaved); return std::string(); }
    if (what == "autosave.last") { out = p.autoLastPath; return std::string(); }
    if (what == "autosave.bytes") { std::error_code ec; const auto n = p.autoLastPath.empty() ? std::uintmax_t{0} : std::filesystem::file_size(p.autoLastPath, ec); out = ec ? "0" : std::to_string(n); return std::string(); }
    if (what == "autosave.folder") { out = p.autoFolder; return std::string(); }
    if (what == "log.lines") {   // the lines of the log file now on the disk (the header counts as one)
        out = "0";
        const std::string path = p.logger ? p.logger->currentPath() : std::string();
        if (!path.empty()) { std::ifstream f(path, std::ios::binary); std::string line; long n = 0; while (std::getline(f, line)) { ++n; } out = std::to_string(n); }
        return std::string();
    }
    if (what == "autosave.note") { out = p.autoNote; return std::string(); }
    if (what == "log.on") { out = p.logger ? "1" : "0"; return std::string(); }
    if (what == "log.written") { out = p.logger ? std::to_string(p.logger->written()) : "0"; return std::string(); }
    if (what == "log.path") { out = p.logger ? p.logger->currentPath() : std::string(); return std::string(); }
    if (what == "log.note") { out = p.logNote; return std::string(); }
    if (what == "zoom.pictures") { out = std::to_string(p.zoomPictures); return std::string(); }
    if (what == "band") { out = p.haveState && p.stack.bandOf(p.st.tunedHz) >= 0 ? bandAt(p.stack.bandOf(p.st.tunedHz)).name : std::string(); return std::string(); }
    if (what == "band.register") { const int b = p.stack.bandOf(p.haveState ? p.st.tunedHz : 0.0); out = b >= 0 ? std::to_string(p.stack.activeRegister(b)) : std::string(); return std::string(); }
    return "s2.<sent|refused|answered|note.<section>|rows.<section>|draft.<id>|fact.<name>|engine.<fft|window|fps|ctun|vfo|vfoother|tuned|mode>|autosave.<on|saved|last|bytes|folder|note>|log.<on|written|path|lines|note>|zoom.pictures|band|band.register>, not " + what;
}

}  // namespace foxsdr::slints2
