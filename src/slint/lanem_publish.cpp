// lanem_publish.cpp - see lanem_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanem_publish.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "lanem_logic.hpp"
#include "plugin_kit.hpp"

namespace foxsdr::slintlanem {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
using nlohmann::json;
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

enum Sec { kSource = 0, kRadio, kAudio, kRecord, kPatch, kAirband, kLevels, kSweep, kDiag, kLibs, kSecCount };
const char* const kSecName[kSecCount] = {"source", "radio", "audio", "record", "patch", "airband", "levels", "sweep", "diag", "libs"};

// what an answer means beyond done / refused
enum Kind { kPlain = 0, kStarted, kCwRead, kLevelRead };

struct Cmd {
    std::uint32_t op = 0;
    long long i0 = 0, i1 = 0;
    double n0 = 0, n1 = 0;
    std::string text;
};
Cmd cmd(std::uint32_t op, long long i0 = 0, const std::string& text = std::string()) { Cmd c; c.op = op; c.i0 = i0; c.text = text; return c; }
}  // namespace

struct LaneMPublisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    std::map<std::string, std::string> draft;     // what the person typed or chose and has not sent (never saved)
    struct Pending { int sec = 0; std::string what; int kind = kPlain; };
    std::map<std::uint64_t, Pending> pending;     // ticket -> section, what it was
    std::map<std::uint64_t, std::string> pendingDraft;   // ticket -> the field whose typed text is dropped once the engine has taken it
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> sig;
    std::array<std::string, kSecCount> lastCount;
    std::map<std::string, std::string> reads;     // the last read-out of an operation that answers in `applied` (cw, level)
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    long lastRebuildTick = -100;
    std::string armed;                            // the id of a destructive key pressed once ("press again")
    std::string loadedAirband;                    // the channel frequency (text) whose stored settings were last loaded into the fields

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {}

    // ---------------------------------------------------------------- facts and drafts
    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    int sel() { const int s = env.selectedRadio ? env.selectedRadio() : 1; return s < 1 ? 1 : s; }
    std::string rn() { return std::to_string(sel()); }
    std::string dv(const std::string& id, const std::string& def = std::string()) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second; }
    int pv(const std::string& id, int def) const { const auto it = draft.find(id); return it == draft.end() ? def : std::atoi(it->second.c_str()); }
    bool bv(const std::string& id, bool def) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second == "1"; }
    static double numOf(const std::string& s, double def = 0.0) { return s.empty() ? def : std::atof(s.c_str()); }

    // ---------------------------------------------------------------- the one place a command leaves
    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    void send(Sec s, const std::string& what, const Cmd& k, int kind = kPlain, const std::string& draftToDrop = std::string()) {
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
            pending[o.ticket] = {static_cast<int>(s), what, kind};
            if (!draftToDrop.empty()) { pendingDraft[o.ticket] = draftToDrop; }
            setNote(s, what + ": " + tr("sent"), kDim);
        }
    }
    void refuseHere(Sec s, const std::string& what, const std::string& why) { ++refused; setNote(s, what + ": " + why, kErr); }
    void sendBuilt(Sec s, const std::string& what, const sll::Built& b, std::uint32_t op, long long i0 = 0, long long i1 = 0, int kind = kPlain) {
        if (!b.ok) { refuseHere(s, what, b.err); return; }
        Cmd c = cmd(op, i0, b.text);
        c.i1 = i1;
        send(s, what, c, kind);
    }

    void onAnswer(const FoxCommandResult& r) {
        const auto it = pending.find(r.ticket);
        if (it == pending.end()) { return; }
        const Sec s = static_cast<Sec>(it->second.sec);
        const std::string what = it->second.what;
        const int kind = it->second.kind;
        pending.erase(it);
        ++answered;
        const auto pd = pendingDraft.find(r.ticket);
        if (pd != pendingDraft.end()) { if (r.status == FOXAPI_OK || r.status == FOXAPI_NO_CHANGE) { draft.erase(pd->second); } pendingDraft.erase(pd); }
        const std::string msg = trim(r.message);
        if (r.status == FOXAPI_OK) {
            if (kind == kCwRead) {
                char b[96];
                std::snprintf(b, sizeof(b), "%.1f Hz, %+.2f", r.applied[0], r.applied[1]);
                reads["cw"] = b;
            } else if (kind == kLevelRead) {
                reads["level"] = bytesText(r.applied[0]);
            }
            setNote(s, what + ": " + (kind == kStarted ? tr("started") : tr("done")) + (msg.empty() ? std::string() : " (" + msg + ")"), kGood);
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
    void choiceD(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int def) {
        choice(v, id, label, opts, pv(id, def), [this, id](int i) { draft[id] = std::to_string(i); });
    }
    void tickD(std::vector<Row>& v, const std::string& id, const std::string& label, bool def, const std::string& tip = std::string()) {
        tick(v, id, label, bv(id, def), [this, id](bool on) { draft[id] = on ? "1" : "0"; }, tip);
    }
    void endNote(std::vector<Row>& v, Sec s) { if (!noteText[s].empty()) { v.push_back(rNote(noteText[s], noteTone[s])); } }
    // a destructive key asks for a second press
    bool confirmTwice(Sec s, const std::string& id, const std::string& what) {
        if (armed != id) { armed = id; setNote(s, what + ": " + tr("press the key again to confirm. This cannot be undone."), kWarn); return false; }
        armed.clear();
        return true;
    }
    void setting(Sec s, const std::string& what, const std::string& k, const std::string& value, const std::string& draftToDrop = std::string()) {
        send(s, what, cmd(FOXAPP_OP_WIREC_SETTING, 0, k + "\x1F" + value), kPlain, draftToDrop);
    }

    // ================================================================ SIGNAL > Source: the I/Q file's transport (row 1)
    void buildSource(std::vector<Row>& v) {
        const std::string n = rn();
        const std::string speedFact = fact("playback." + n + ".speed");
        if (speedFact.empty()) { return; }   // the selected radio does not play a file: nothing is drawn
        const bool paused = fact("playback." + n + ".paused") == "1";
        const std::string policy = fact("playback." + n + ".policy");
        v.push_back(rHeading(tr("I/Q file playback")));
        v.push_back(rReadout(tr("Speed"), speedText(numOf(speedFact, 1.0)) + "  " + policyWords(policy)));
        v.push_back(rReadout(tr("Playback"), paused ? tr("paused") : tr("playing")));
        if (env.filePositionSec) { char b[48]; std::snprintf(b, sizeof(b), "%.1f s", env.filePositionSec()); v.push_back(rReadout(tr("Position"), b)); }
        field(v, "p.speed", tr("Playback speed"), speedFact, tr("0.25 to 8; 1 is the speed it was recorded at"));
        key(v, "p.speed-set", tr("Set the speed"), [this] {
            double sp = 0; std::string e;
            if (!parseSpeed(dv("p.speed", "1"), sp, e)) { refuseHere(kSource, tr("Set the speed"), e); return; }
            Cmd c = cmd(FOXAPP_OP_FILE_TRANSPORT, 0); c.n0 = sp; send(kSource, tr("Set the speed"), c);
        }, true, tr("Varispeed multiplies every frequency in the recording by the speed, like a tape. Block skip keeps the frequencies and chops the time. At speed 1 the two are the same. Not remembered: a file opened again plays at speed 1."));
        choice(v, "p.policy", tr("Speed policy"), {tr("Varispeed"), tr("Block skip")}, policy == "blockskip" ? 1 : 0, [this](int i) { Cmd c = cmd(FOXAPP_OP_FILE_TRANSPORT, 1); c.i1 = i == 1 ? 1 : 0; send(kSource, tr("Speed policy"), c); });
        key(v, "p.pause", tr("Pause"), [this] { send(kSource, tr("Pause"), cmd(FOXAPP_OP_FILE_TRANSPORT, 2)); }, !paused);
        key(v, "p.resume", tr("Resume"), [this] { send(kSource, tr("Resume"), cmd(FOXAPP_OP_FILE_TRANSPORT, 3)); }, paused);
        field(v, "p.step", tr("Blocks to step"), "1", tr("1 to 4096; a block is 4096 frames"));
        const auto step = [this](int sign) {
            long long b = 0; std::string e;
            if (!parseStepBlocks(dv("p.step", "1"), b, e)) { refuseHere(kSource, tr("Step"), e); return; }
            b = std::llabs(b) * sign;
            Cmd c = cmd(FOXAPP_OP_FILE_TRANSPORT, 4); c.i1 = b; send(kSource, sign < 0 ? tr("Step back") : tr("Step forward"), c);
        };
        key(v, "p.step-back", tr("Step back"), [step] { step(-1); }, paused, tr("Only while paused. Goes back that many blocks and plays one block from there."));
        key(v, "p.step-fwd", tr("Step forward"), [step] { step(1); }, paused, tr("Only while paused."));
        endNote(v, kSource);
    }

    // ================================================================ SIGNAL > Radio: the receive clarifier (row 2) and the symbol rate (row 20)
    void buildRadio(std::vector<Row>& v) {
        const std::string n = rn();
        v.push_back(rHeading(tr("Receive clarifier (RIT)")));
        v.push_back(rNote(tr("Shifts what you HEAR by an offset while the dial, the bookmarks and the CAT answer stay where they are. It is never saved: it is off at every start.")));
        const bool on = fact("rit." + n + ".on") == "1";
        const double hz = numOf(fact("rit." + n + ".hz"));
        const double lim = numOf(fact("rit." + n + ".max"), 9999.0);
        const std::string clearAt = fact("rit." + n + ".clearOnRetune");
        v.push_back(rReadout(tr("Clarifier"), on ? tr("on") + ", " + signedHzText(hz) : tr("off") + ", " + signedHzText(hz)));
        tick(v, "c.on", tr("Clarifier on"), on, [this, hz](bool o) { Cmd c = cmd(FOXAPP_OP_RIT, 0); c.n0 = hz; c.i1 = o ? 1 : 0; send(kRadio, tr("Clarifier"), c); });
        // the rest is folded away until asked for: the Radio section is open at every start and a long list would push the sections under it off the screen
        const bool more = bv("c.open", false);
        key(v, "c.more", more ? tr("Hide the clarifier controls") : tr("More clarifier controls"), [this, more] { draft["c.open"] = more ? "0" : "1"; });
        if (more) {
        field(v, "c.hz", tr("Clarifier offset (Hz)"), numberText(hz), tr("Inside the limit below (9999 Hz unless you changed it); negative lowers what you hear"));
        key(v, "c.set", tr("Set the clarifier"), [this, lim] {
            double h = 0; std::string e;
            if (!parseRitHz(dv("c.hz", "0"), lim, h, e)) { refuseHere(kRadio, tr("Set the clarifier"), e); return; }
            Cmd c = cmd(FOXAPP_OP_RIT, 0); c.n0 = h; c.i1 = 1; send(kRadio, tr("Set the clarifier"), c, kPlain, "c.hz");
        });
        static const int steps[4] = {-100, -10, 10, 100};
        for (const int s : steps) {
            const std::string label = (s > 0 ? "+" : "") + std::to_string(s) + " Hz";
            key(v, "c.step" + std::to_string(s), label, [this, s, label] { Cmd c = cmd(FOXAPP_OP_RIT, 1); c.n0 = s; send(kRadio, label, c); }, true, tr("Adds to the offset and switches the clarifier on."));
        }
        key(v, "c.clear", tr("Clear the clarifier"), [this] { send(kRadio, tr("Clear the clarifier"), cmd(FOXAPP_OP_RIT, 2)); }, true, tr("Back to no offset, and off."));
        field(v, "c.max", tr("Largest offset (Hz)"), numberText(lim), tr("1 to 20000"));
        field(v, "c.clear-at", tr("Clear on a retune of (Hz)"), clearAt.empty() ? "0" : clearAt, tr("A retune larger than this clears the clarifier; 0 = never"));
        key(v, "c.limits", tr("Set the limits"), [this] {
            double a = 0, b = 0; std::string e;
            if (!parseRitLimits(dv("c.max", "9999"), dv("c.clear-at", "0"), a, b, e)) { refuseHere(kRadio, tr("Set the limits"), e); return; }
            Cmd c = cmd(FOXAPP_OP_RIT, 3); c.n0 = a; c.n1 = b; send(kRadio, tr("Set the limits"), c);
        });
        }

        v.push_back(rHeading(tr("Symbol rate")));
        const std::string sr = fact("wirec.sig.symbol_rate");
        v.push_back(rField("c.baud", tr("Symbol rate (baud)"), dv("c.baud", sr.empty() ? "0" : sr), tr("The rate of the signal you are looking at, for a decoder that asks for it. 0 = none set. FoxSDR never measures it.")));
        handlers["c.baud"] = [this](const std::string& t) {
            draft["c.baud"] = t;
            double b = 0; std::string e;
            if (!parseSymbolRate(t, b, e)) { refuseHere(kRadio, tr("Symbol rate"), e); return; }
            setting(kRadio, tr("Symbol rate"), "sig.symbol_rate", symbolRateText(b), "c.baud");
        };
        endNote(v, kRadio);
    }

    // ================================================================ SIGNAL > Audio filters: CW surround (row 3)
    void buildAudio(std::vector<Row>& v) {
        const std::string n = rn();
        const std::string p = "cwsurround." + n + ".";
        v.push_back(rHeading(tr("CW surround")));
        v.push_back(rNote(tr("Places a CW signal between your ears by its pitch: a lower pitch to the left, a higher one to the right. Heard only in CW mode on a mono receiver with no stereo placement of its own.")));
        const bool on = fact(p + "on") == "1";
        const bool inv = fact(p + "invert") == "1";
        tick(v, "w.on", tr("CW surround"), on, [this](bool o) { send(kAudio, tr("CW surround"), cmd(FOXAPP_OP_CW_SURROUND, 0, std::string("{\"enabled\":") + (o ? "true" : "false") + "}")); }, tr("Off until you tick it. Nothing is saved: a restart is off again."));
        tick(v, "w.inv", tr("Invert left and right"), inv, [this](bool o) { send(kAudio, tr("Invert left and right"), cmd(FOXAPP_OP_CW_SURROUND, 0, std::string("{\"invert\":") + (o ? "true" : "false") + "}")); });
        const std::string c = fact(p + "centre"), sp = fact(p + "spread"), ra = fact(p + "ratio"), ho = fact(p + "hold");
        field(v, "w.centre", tr("Centre pitch (Hz)"), c.empty() ? "700" : c, tr("200 to 1500: the pitch that sits in the middle"));
        field(v, "w.spread", tr("Spread (Hz)"), sp.empty() ? "400" : sp, tr("50 to 2000: how far a pitch can lie from the centre before it is all the way to one side"));
        field(v, "w.ratio", tr("Tone ratio"), ra.empty() ? "100" : ra, tr("2 to 1000"));
        field(v, "w.hold", tr("Hold (ms)"), ho.empty() ? "400" : ho, tr("0 to 5000: how long the last pitch is kept between dots"));
        key(v, "w.apply", tr("Apply the CW surround settings"), [this, on, inv] {
            sendBuilt(kAudio, tr("CW surround"), cwSurroundJson(on, dv("w.centre", "700"), dv("w.spread", "400"), inv, dv("w.ratio", "100"), dv("w.hold", "400")), FOXAPP_OP_CW_SURROUND, 0);
        });
        key(v, "w.read", tr("Read the pitch now"), [this] { send(kAudio, tr("Read the pitch"), cmd(FOXAPP_OP_CW_SURROUND, 1), kCwRead); }, true, tr("The pitch the engine has estimated and where it is placing it (-1 left to +1 right)."));
        const auto rd = reads.find("cw");
        if (rd != reads.end()) { v.push_back(rReadout(tr("Pitch and placement"), rd->second)); }
        endNote(v, kAudio);
    }

    // ================================================================ SIGNAL > Recorder: the I/Q take's file kind (row 4)
    void buildRecord(std::vector<Row>& v) {
        v.push_back(rHeading(tr("I/Q take file")));
        const std::string k = fact("record.iqcontainer");
        choice(v, "k.container", tr("I/Q take file"), {"WAV", "RF64", "SigMF"}, k == "sigmf" ? 2 : k == "rf64" ? 1 : 0, [this](int i) { send(kRecord, tr("I/Q take file"), cmd(FOXAPP_OP_RECORD_IQ_CONTAINER, i)); },
               tr("WAV ends a take at 4 GiB. RF64 has no such end. SigMF writes the samples and a metadata file beside them that records every retune. The next take starts in the kind you choose; a take under way keeps its own."));
        v.push_back(rNote(tr("Kept for next time. The audio file format (WAV or FLAC) is under Recording and files.")));
        endNote(v, kRecord);
    }

    // ================================================================ SIGNAL > Patch: the edit history, copy, paste, select, bypass (rows 5 and 6)
    void buildPatch(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Patch editing")));
        const std::string un = fact("patchedit.undo"), re = fact("patchedit.redo"), selection = fact("patchedit.selection"), clip = fact("patchedit.clipboard");
        v.push_back(rReadout(tr("Steps to undo / redo"), (un.empty() ? "0" : un) + " / " + (re.empty() ? "0" : re)));
        key(v, "q.undo", tr("Undo"), [this] { send(kPatch, tr("Undo"), cmd(FOXAPP_OP_PATCH_UNDO)); }, true, tr("Goes back one step of the patch. The radios keep their own state."));
        key(v, "q.redo", tr("Redo"), [this] { send(kPatch, tr("Redo"), cmd(FOXAPP_OP_PATCH_REDO)); });
        v.push_back(rReadout(tr("Selected parts"), selection.empty() ? tr("none") : selection));
        field(v, "q.ids", tr("Part numbers"), "", tr("The numbers of parts of the patch, separated by commas or spaces"));
        const auto selectOp = [this](int mode, const std::string& what) {
            sendBuilt(kPatch, what, patchIdsJson(dv("q.ids")), FOXAPP_OP_PATCH_SELECT, mode);
        };
        key(v, "q.sel", tr("Select these parts"), [selectOp] { selectOp(0, tr("Select these parts")); }, !trim(dv("q.ids")).empty());
        key(v, "q.sel-add", tr("Add these to the selection"), [selectOp] { selectOp(1, tr("Add these to the selection")); }, !trim(dv("q.ids")).empty());
        key(v, "q.sel-tog", tr("Toggle these in the selection"), [selectOp] { selectOp(2, tr("Toggle these in the selection")); }, !trim(dv("q.ids")).empty());
        key(v, "q.sel-all", tr("Select all"), [this] { send(kPatch, tr("Select all"), cmd(FOXAPP_OP_PATCH_SELECT, 4)); });
        key(v, "q.sel-none", tr("Select none"), [this] { send(kPatch, tr("Select none"), cmd(FOXAPP_OP_PATCH_SELECT, 3)); });
        key(v, "q.copy", tr("Copy the selection"), [this] { send(kPatch, tr("Copy the selection"), cmd(FOXAPP_OP_PATCH_COPY)); });
        v.push_back(rReadout(tr("Parts on the clipboard"), clip.empty() ? "0" : clip));
        field(v, "q.px", tr("Paste at x"), "40", tr("Where the top-left of the pasted set goes, in the patch's own units"));
        field(v, "q.py", tr("Paste at y"), "40");
        key(v, "q.paste", tr("Paste"), [this] {
            double x = 0, y = 0; std::string e;
            if (!parsePastePoint(dv("q.px", "40"), dv("q.py", "40"), x, y, e)) { refuseHere(kPatch, tr("Paste"), e); return; }
            Cmd c = cmd(FOXAPP_OP_PATCH_PASTE); c.n0 = x; c.n1 = y; send(kPatch, tr("Paste"), c);
        }, true, tr("The pasted parts get new numbers and become the selection."));
        key(v, "q.bypass", tr("Bypass the selection"), [this] { Cmd c = cmd(FOXAPP_OP_PATCH_BYPASS, 1); send(kPatch, tr("Bypass the selection"), c); }, true, tr("A bypassed part is skipped by what runs and stays in the patch. A part whose first input and output carry different things cannot be bypassed."));
        key(v, "q.unbypass", tr("Stop bypassing the selection"), [this] { Cmd c = cmd(FOXAPP_OP_PATCH_BYPASS, 0); send(kPatch, tr("Stop bypassing the selection"), c); });
        v.push_back(rNote(tr("These work on the engine's copy of the patch. Pick parts by number here: a click on the canvas does not select in the engine yet.")));
        endNote(v, kPatch);
    }

    // ================================================================ TOOLS > Airband: per-channel settings (row 7)
    void airbandLoad(const std::string& freqText) {
        long long hz = 0; std::string e;
        if (!airbandHz(freqText, hz, e)) { refuseHere(kAirband, tr("Load the channel's settings"), e); return; }
        const std::string line = fact("airband.channel." + std::to_string(hz));
        AirbandFields f;
        if (line.empty() || !airbandFieldsFrom(line, f)) { setNote(kAirband, tr("Load the channel's settings") + ": " + tr("this channel has no settings of its own; it uses the shared squelch"), kDim); return; }
        const auto put = [this](const char* id, const std::string& val) { draft[id] = val; };
        put("a.bw", f.bandwidthKHz); put("a.mode", std::to_string(f.mode)); put("a.sqkind", std::to_string(f.squelchKind));
        put("a.level", f.levelDb); put("a.snr", f.snrOpenDb); put("a.hyst", f.snrHystDb); put("a.hold", f.holdMs);
        put("a.hp", f.highPassHz); put("a.lp", f.lowPassHz); put("a.order", f.filterOrder == 2 ? "0" : "1"); put("a.notch", f.notches);
        put("a.out", std::to_string(f.output)); put("a.daily", f.dailyFolder ? "1" : "0"); put("a.mb", f.maxFileMb); put("a.port", f.udpPort);
        setNote(kAirband, tr("Load the channel's settings") + ": " + tr("done"), kGood);
    }
    AirbandFields airbandDraft() const {
        AirbandFields f;
        f.freqMHz = dv("a.freq");
        f.bandwidthKHz = dv("a.bw", "10");
        f.mode = pv("a.mode", 0);
        f.squelchKind = pv("a.sqkind", 0);
        f.levelDb = dv("a.level", "-50"); f.snrOpenDb = dv("a.snr", "10"); f.snrHystDb = dv("a.hyst", "3"); f.holdMs = dv("a.hold", "400");
        f.highPassHz = dv("a.hp", "0"); f.lowPassHz = dv("a.lp", "0");
        f.filterOrder = pv("a.order", 1) == 0 ? 2 : 4;
        f.notches = dv("a.notch");
        f.output = pv("a.out", 0);
        f.dailyFolder = bv("a.daily", true);
        f.maxFileMb = dv("a.mb", "100"); f.udpPort = dv("a.port", "0");
        return f;
    }
    void buildAirband(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Settings of one airband channel")));
        v.push_back(rNote(tr("A channel with no settings of its own uses the monitor's shared squelch. Outputs are off until you choose one: a file goes into the recordings folder, a stream goes to this computer only.")));
        const std::string count = fact("airband.channels");
        v.push_back(rReadout(tr("Channels with settings of their own"), count.empty() ? "0" : count));
        const bool open = bv("a.open", false);   // the editor is folded away until asked for: the Airband section is open at every start
        key(v, "a.more", open ? tr("Hide the channel editor") : tr("Edit this channel's own settings"), [this, open] { draft["a.open"] = open ? "0" : "1"; });
        if (!open) { endNote(v, kAirband); return; }
        field(v, "a.freq", tr("Channel frequency (MHz)"), "", tr("The frequency of a channel in the monitor's list, for example 118.1"));
        key(v, "a.load", tr("Load this channel's settings"), [this] { airbandLoad(dv("a.freq")); }, !trim(dv("a.freq")).empty(), tr("Fills the fields below with what the engine holds for that frequency."));
        v.push_back(rHeading(tr("Channel")));
        field(v, "a.bw", tr("Channel width (kHz)"), "10", tr("1 to 200"));
        choiceD(v, "a.mode", tr("Channel mode"), {tr("AM channel"), tr("NFM channel")}, 0);
        v.push_back(rHeading(tr("Squelch")));
        choiceD(v, "a.sqkind", tr("Squelch kind"), {tr("Level"), tr("Signal to noise")}, 0);
        field(v, "a.level", tr("Squelch level (dB)"), "-50", tr("-120 to 0: the channel's power below which it is silent (level squelch)"));
        field(v, "a.snr", tr("Squelch opens at (dB SNR)"), "10", tr("1 to 60 (signal-to-noise squelch)"));
        field(v, "a.hyst", tr("Squelch hysteresis (dB)"), "3", tr("0 to 30 (signal-to-noise squelch)"));
        field(v, "a.hold", tr("Squelch hold (ms)"), "400", tr("0 to 10000: how long it stays open after the signal falls away"));
        v.push_back(rHeading(tr("Audio filter")));
        field(v, "a.hp", tr("High-pass (Hz, 0 = off)"), "0", tr("20 to 3000"));
        field(v, "a.lp", tr("Low-pass (Hz, 0 = off)"), "0", tr("300 to 20000, above the high-pass"));
        choiceD(v, "a.order", tr("Filter order"), {"2", "4"}, 1);
        field(v, "a.notch", tr("Notches"), "", tr("Up to three, separated by semicolons: Hz, a slash and Q (for example 1000/30; 2000)"));
        v.push_back(rHeading(tr("Output")));
        choiceD(v, "a.out", tr("Output"), {tr("None"), tr("A WAV file"), tr("A UDP stream")}, 0);
        tickD(v, "a.daily", tr("Put the file in a folder for the day"), true);
        field(v, "a.mb", tr("File size limit (MB)"), "100", tr("1 to 2000"));
        field(v, "a.port", tr("Stream port"), "0", tr("1024 to 65535; the stream goes to 127.0.0.1 only"));
        key(v, "a.set", tr("Set this channel's settings"), [this] {
            const AirbandBuilt b = airbandChannelText(airbandDraft());
            if (!b.ok) { refuseHere(kAirband, tr("Set this channel's settings"), b.err); return; }
            send(kAirband, tr("Set this channel's settings"), cmd(FOXAPP_OP_AIRBAND_CHANNEL, 0, b.text));
        });
        key(v, "a.clear", tr("Clear this channel's settings"), [this] {
            long long hz = 0; std::string e;
            if (!airbandHz(dv("a.freq"), hz, e)) { refuseHere(kAirband, tr("Clear this channel's settings"), e); return; }
            Cmd c = cmd(FOXAPP_OP_AIRBAND_CHANNEL, 1); c.n0 = static_cast<double>(hz); send(kAirband, tr("Clear this channel's settings"), c);
        }, !trim(dv("a.freq")).empty());
        key(v, "a.clear-all", armed == "a.clear-all" ? tr("Press again to clear every channel's settings") : tr("Clear every channel's settings"), [this] {
            if (!confirmTwice(kAirband, "a.clear-all", tr("Clear every channel's settings"))) { return; }
            send(kAirband, tr("Clear every channel's settings"), cmd(FOXAPP_OP_AIRBAND_CHANNEL, 2));
        }, true, tr("Every channel goes back to the shared squelch. The key asks you to press it a second time."));
        const std::string line = [&] { long long hz = 0; std::string e; if (!airbandHz(dv("a.freq"), hz, e)) { return std::string(); } return fact("airband.channel." + std::to_string(hz)); }();
        if (!line.empty()) { v.push_back(rReadout(tr("Held by the engine"), line)); }
        endNote(v, kAirband);
    }

    // ================================================================ TOOLS > Export: the level store (row 8)
    void buildLevels(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Level store")));
        v.push_back(rNote(tr("Keeps the last hour of every open radio's levels in memory (one row every quarter of a second), so a radio on another page still has its history when you look. Nothing is written to disk. Off until you turn it on.")));
        const bool on = fact("levels.on") == "1";
        const std::string cap = fact("levels.capMb");
        tick(v, "v.on", tr("Keep levels of every open radio"), on, [this, on](bool o) {
            if (!o) { send(kLevels, tr("Level store"), cmd(FOXAPP_OP_LEVEL_STORE, 0)); return; }
            double mb = 0; std::string e;
            if (!parseCapMiB(dv("v.cap", "16"), mb, e)) { refuseHere(kLevels, tr("Level store"), e); return; }
            Cmd c = cmd(FOXAPP_OP_LEVEL_STORE, 1); c.n0 = mb; send(kLevels, tr("Level store"), c);
            (void)on;
        }, tr("Off by default. Turning it off frees the memory."));
        field(v, "v.cap", tr("Memory per radio (MiB)"), cap.empty() ? "16" : cap, tr("1 to 64. An hour of 1024-bin rows is about 15 MB a radio, so eight radios an hour each are about 121 MB. Used when you turn the store on: change it with the store off."));
        key(v, "v.clear", tr("Clear every ring"), [this] { send(kLevels, tr("Clear every ring"), cmd(FOXAPP_OP_LEVEL_STORE, 2)); }, on, tr("Empties the stored levels of every radio; the store keeps running."));
        key(v, "v.read", tr("Read this radio's figures"), [this] { send(kLevels, tr("Read this radio's figures"), cmd(FOXAPP_OP_LEVEL_STORE, 3), kLevelRead); }, on);
        const std::string n = rn();
        const std::string rows = fact("levels." + n + ".rows");
        if (!rows.empty()) {
            v.push_back(rReadout(tr("Rows stored"), rows));
            v.push_back(rReadout(tr("Time held (s)"), fact("levels." + n + ".seconds")));
            v.push_back(rReadout(tr("Memory used"), bytesText(numOf(fact("levels." + n + ".bytes")))));
            v.push_back(rReadout(tr("Bins per row"), fact("levels." + n + ".bins")));
            v.push_back(rReadout(tr("Segments (retunes)"), fact("levels." + n + ".segments")));
        }
        endNote(v, kLevels);
    }

    // ================================================================ SYSTEM > Sweep: the stitched sweep (rows 11, 12) and the history (rows 13, 14)
    void buildSweep(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Sweep one of your radios")));
        v.push_back(rNote(tr("Steps the radio you choose across a wider range, one centre after another, joins the pieces into one line and puts the radio's own centre back when the run ends. The radio must be running. Receive only. Nothing starts until you press the key.")));
        field(v, "x.radio", tr("Radio to sweep"), rn(), tr("1 to 8: the number of an open radio"));
        field(v, "x.start", tr("Radio sweep start (MHz)"), "");
        field(v, "x.stop", tr("Radio sweep stop (MHz)"), "", tr("Above the start; a range that needs more than 512 centres at the radio's rate is refused"));
        field(v, "x.step", tr("Step (part of the span)"), "0.6", tr("0.3 to 0.7: how far the radio moves each time, as a part of what it sees at once"));
        field(v, "x.trim", tr("Edge trimmed (part)"), "0.12", tr("0 to 0.4: the part of each end of a piece that is thrown away"));
        field(v, "x.settle", tr("Settle time (ms)"), "150", tr("0 to 5000: the wait after each retune"));
        field(v, "x.frames", tr("Frames averaged"), "2", tr("1 to 64"));
        field(v, "x.timeout", tr("Give up after (ms)"), "4000", tr("500 to 60000"));
        field(v, "x.passes", tr("Passes"), "1", tr("0 to 1000000; 0 = until you stop it"));
        key(v, "x.go", tr("Sweep this radio"), [this] {
            sendBuilt(kSweep, tr("Sweep this radio"), stitchedSweepJson(dv("x.radio", rn()), dv("x.start"), dv("x.stop"), dv("x.step", "0.6"), dv("x.trim", "0.12"), dv("x.settle", "150"), dv("x.frames", "2"), dv("x.timeout", "4000"), dv("x.passes", "1")), FOXAPP_OP_SWEEP_OPEN);
        }, true, tr("Retunes your own radio, only now that you pressed this key."));
        key(v, "x.stop-key", tr("Stop the stitched sweep"), [this] { send(kSweep, tr("Stop the stitched sweep"), cmd(FOXAPP_OP_SWEEP_CLOSE)); }, true, tr("Ends the run and puts the radio's own centre back."));
        const std::string stitch = fact("sweep.stitch");
        const std::string prog = stitchProgress(stitch);
        if (!stitch.empty()) {
            v.push_back(rReadout(tr("Stitched sweep"), prog.empty() ? tr("not running") : tr("running") + ", " + prog));
            const std::string note = sll::jsonString(stitch, "note");
            if (!note.empty()) { v.push_back(rNote(note, kWarn)); }
        }

        v.push_back(rHeading(tr("Sweep history")));
        const std::string hist = fact("sweep.history");
        const std::string ro = historyReadout(hist);
        v.push_back(rReadout(tr("History kept"), ro.empty() ? tr("nothing yet") : ro));
        std::string hnote = sll::jsonString(hist, "note");
        if (hnote.empty()) { hnote = fact("sweep.history.note"); }
        if (!hnote.empty()) { v.push_back(rNote(hnote)); }
        field(v, "x.hpath", tr("History file"), "", tr("The full path of the file to write or read. A saved file is plain text; a file that exists is never overwritten."));
        choiceD(v, "x.hfmt", tr("History format"), {"CSV", "JSON", "XML"}, 0);
        key(v, "x.hsave", tr("Save the history..."), [this] { sendBuilt(kSweep, tr("Save the history"), sweepHistoryJson("save", dv("x.hpath"), pv("x.hfmt", 0)), FOXAPP_OP_SWEEP_HISTORY); }, !trim(dv("x.hpath")).empty());
        key(v, "x.hload", tr("Load a history..."), [this] { sendBuilt(kSweep, tr("Load a history"), sweepHistoryJson("load", dv("x.hpath"), 0), FOXAPP_OP_SWEEP_HISTORY); }, !trim(dv("x.hpath")).empty(), tr("Replaces the history in memory; refused while a sweep is running."));
        key(v, "x.hclear", armed == "x.hclear" ? tr("Press again to clear the history") : tr("Clear the history"), [this] {
            if (!confirmTwice(kSweep, "x.hclear", tr("Clear the history"))) { return; }
            sendBuilt(kSweep, tr("Clear the history"), sweepHistoryJson("clear", "", 0), FOXAPP_OP_SWEEP_HISTORY);
        }, true, tr("Empties the sweeps kept in memory. The key asks you to press it a second time."));
        endNote(v, kSweep);
    }

    // ================================================================ SYSTEM > Diagnostics: the spectrum's GPU counters (row 15)
    void buildDiag(std::vector<Row>& v) {
        const GpuCounters g = parseGpuCounters(fact("radio." + rn() + ".gpufft"));
        if (!g.have) { return; }
        v.push_back(rHeading(tr("Spectrum on the GPU")));
        v.push_back(rReadout(tr("Standard spectrum runs on"), g.backend.empty() ? tr("the CPU") : (g.backend == "gpu" ? tr("the GPU") : tr("the CPU")) + (g.engaged ? "" : "  (" + tr("not engaged") + ")")));
        if (g.hasBlock) {
            v.push_back(rReadout(tr("Spectrum block, frames on the GPU"), numberText(g.blockGpu)));
            v.push_back(rReadout(tr("Spectrum block, frames on the CPU"), numberText(g.blockCpu)));
            v.push_back(rReadout(tr("Spectrum block's GPU"), g.blockGpuUp ? tr("up") : tr("not in use")));
        }
        if (!g.error.empty()) { v.push_back(rNote(g.error, kWarn)); }
        v.push_back(rNote(tr("CPU, Auto or GPU is chosen under Measurement, GPU spectrum. One switch drives both paths.")));
    }

    // ================================================================ SYSTEM > Needed libraries: a plugin's request to open a page or a folder (row 19)
    void buildLibs(std::vector<Row>& v) {
        const int count = std::atoi(fact("plugin.open.count").c_str());
        const std::string last = fact("plugin.open.last");
        if (count <= 0) { if (!last.empty()) { v.push_back(rHeading(tr("A plugin's requests"))); v.push_back(rNote(last)); } return; }
        v.push_back(rHeading(tr("A plugin asks you to open something")));
        v.push_back(rNote(tr("Nothing opens by itself. A plugin has asked for the page or the folder below; it opens only when you press its key.")));
        for (int i = 1; i <= std::min(count, 16); ++i) {
            OpenRequest r;
            if (!parseOpenRequest(fact("plugin.open." + std::to_string(i)), r)) { continue; }
            const bool isUrl = r.kind == "url";
            v.push_back(rReadout(tr("Asked by"), r.pluginName));
            v.push_back(rReadout(isUrl ? tr("Address") : tr("Folder"), r.target));
            const std::string id = "z.open" + std::to_string(i);
            const std::string text = openConfirmText(r);
            key(v, id, (isUrl ? tr("Open the download page") : tr("Open that folder")) + " (" + r.pluginName + ")", [this, text, isUrl] {
                send(kLibs, isUrl ? tr("Open the download page") : tr("Open that folder"), cmd(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, 0, text));
            }, true, isUrl ? tr("Opens that address in your browser.") : tr("Opens the plugin's own folder for libraries in the file manager."));
        }
        if (!last.empty()) { v.push_back(rNote(last)); }
        endNote(v, kLibs);
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
        auto& l = env.ui->global<LaneM>();
        switch (s) {
            case kSource: l.set_rows_source(models[s]); break;
            case kRadio: l.set_rows_radio(models[s]); break;
            case kAudio: l.set_rows_audio(models[s]); break;
            case kRecord: l.set_rows_record(models[s]); break;
            case kPatch: l.set_rows_patch(models[s]); break;
            case kAirband: l.set_rows_airband(models[s]); break;
            case kLevels: l.set_rows_levels(models[s]); break;
            case kSweep: l.set_rows_sweep(models[s]); break;
            case kDiag: l.set_rows_diag(models[s]); break;
            case kLibs: l.set_rows_libs(models[s]); break;
            default: break;
        }
    }

    void rebuildAll() {
        dirty = false;
        using Builder = void (Impl::*)(std::vector<Row>&);
        static const Builder builders[kSecCount] = {&Impl::buildSource, &Impl::buildRadio, &Impl::buildAudio, &Impl::buildRecord, &Impl::buildPatch, &Impl::buildAirband, &Impl::buildLevels, &Impl::buildSweep, &Impl::buildDiag, &Impl::buildLibs};
        handlers.clear();
        for (int s = 0; s < kSecCount; ++s) {
            std::vector<Row> rows;
            (this->*builders[s])(rows);
            publishSection(static_cast<Sec>(s), rows);
        }
    }

    void act(const std::string& id, const std::string& text) {
        // a destructive key armed by its first press is disarmed by any other action
        if (!armed.empty() && armed != id) { armed.clear(); noteText[kAirband].clear(); noteText[kSweep].clear(); }
        const auto it = handlers.find(id);
        if (it == handlers.end()) { return; }
        const Handler h = it->second;
        h(text);
        rebuildAll();
    }
};

LaneMPublisher::LaneMPublisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
LaneMPublisher::~LaneMPublisher() = default;

void LaneMPublisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneM>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void LaneMPublisher::tick(long tickNo) {
    // about twice a second the rows are read again from the facts (a few get_setting calls per section); at once after any action or answer
    if (p_->dirty || tickNo - p_->lastRebuildTick >= 15) {
        p_->lastRebuildTick = tickNo;
        p_->rebuildAll();
    }
}

void LaneMPublisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

std::string LaneMPublisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "lm.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "lm.rows.<section>, not " + sec;
    }
    if (what.rfind("draft.", 0) == 0) { out = p.dv(what.substr(6)); return std::string(); }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    if (what.rfind("read.", 0) == 0) { const auto it = p.reads.find(what.substr(5)); out = it == p.reads.end() ? std::string() : it->second; return std::string(); }
    return "lm.<sent|refused|answered|note.<section>|rows.<section>|draft.<id>|fact.<name>|read.<what>>, not " + what;
}

}  // namespace foxsdr::slintlanem
