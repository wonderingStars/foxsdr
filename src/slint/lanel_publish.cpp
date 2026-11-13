// lanel_publish.cpp - see lanel_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanel_publish.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanek_logic.hpp"
#include "lanel_logic.hpp"
#include "plugin_kit.hpp"

namespace foxsdr::slintlanel {

namespace {
namespace slk = foxsdr::slintlanek;
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

std::filesystem::path pathOf(const std::string& u8) { return std::filesystem::path(std::u8string(u8.begin(), u8.end())); }

// A file the person named, read whole (at most `cap` bytes). `err` is a sentence (translated) when it cannot be.
bool readWholeFile(const std::string& path, std::size_t cap, std::string& out, std::string& err) {
    out.clear();
    const std::string p = trim(path);
    if (p.empty()) { err = tr("type the full path of the file"); return false; }
    std::error_code ec;
    const std::filesystem::path fp = pathOf(p);
    if (!fp.is_absolute()) { err = tr("that is not a full path"); return false; }
    const auto size = std::filesystem::file_size(fp, ec);
    if (ec) { err = tr("that file cannot be read"); return false; }
    if (size > cap) { err = tr("that file is too large for this operation"); return false; }
    std::ifstream f(fp, std::ios::binary);
    if (!f) { err = tr("that file cannot be read"); return false; }
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

enum Sec { kTuning = 0, kScan, kRecord, kMeasure, kSweep, kSources, kOutputs, kLibs, kSystem, kWeb, kHam, kSecCount };
const char* const kSecName[kSecCount] = {"tuning", "scan", "record", "measure", "sweep", "sources", "outputs", "libs", "system", "web", "ham"};

struct Cmd {
    std::uint32_t op = 0;
    long long i0 = 0, i1 = 0;
    double n0 = 0, n1 = 0, n2 = 0, n3 = 0;
    std::string text;
};
Cmd cmd(std::uint32_t op, long long i0 = 0, const std::string& text = std::string()) { Cmd c; c.op = op; c.i0 = i0; c.text = text; return c; }
}  // namespace

struct LaneLPublisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    using Handler = std::function<void(const std::string&)>;
    std::map<std::string, Handler> handlers;
    std::map<std::string, std::string> draft;     // what the person typed or chose and has not sent (never saved)
    std::map<std::uint64_t, std::pair<int, std::string>> pending;   // ticket -> section, what it was
    std::array<std::string, kSecCount> noteText;
    std::array<int, kSecCount> noteTone{};
    std::array<std::string, kSecCount> sig;
    long sent = 0, refused = 0, answered = 0;
    bool dirty = true;
    long lastRebuildTick = -100;
    std::string armedClear;                       // the id of a destructive key pressed once ("press again")
    std::string lastMeasure, lastBits;            // which result the Measurement section shows
    std::vector<NeededRow> libs;                  // the last parse of the needed-libraries fact (for the drive scripts)
    std::array<std::string, kSecCount> lastCount;

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session) {}

    // ---------------------------------------------------------------- facts and drafts
    std::string fact(const std::string& name) { std::string s; if (!table.fact("fact." + name, s)) { s.clear(); } return s; }
    int sel() { const int s = env.selectedRadio ? env.selectedRadio() : 1; return s < 1 ? 1 : s; }
    std::string selFact(const char* tail) { return fact("radio." + std::to_string(sel()) + "." + tail); }
    std::string dv(const std::string& id, const std::string& def = std::string()) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second; }
    int pv(const std::string& id, int def) const { const auto it = draft.find(id); return it == draft.end() ? def : std::atoi(it->second.c_str()); }
    bool bv(const std::string& id, bool def) const { const auto it = draft.find(id); return it == draft.end() ? def : it->second == "1"; }

    // ---------------------------------------------------------------- the one place a command leaves
    void setNote(Sec s, const std::string& t, int tone) { noteText[s] = t; noteTone[s] = tone; dirty = true; }

    void send(Sec s, const std::string& what, const Cmd& k, std::uint32_t radio = 0) {
        if (!env.submit) { return; }
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = k.op;
        c.ival[0] = k.i0;
        c.ival[1] = k.i1;
        c.num[0] = k.n0; c.num[1] = k.n1; c.num[2] = k.n2; c.num[3] = k.n3;
        const std::string* longp = nullptr;
        if (k.text.size() < sizeof(c.text)) { std::memcpy(c.text, k.text.data(), k.text.size()); } else { longp = &k.text; }
        const SubmitOutcome o = env.submit(c, radio, longp);
        ++sent;
        if (o.ticket == 0 || slintlanelRefusal(o.status)) {
            ++refused;
            setNote(s, what + ": " + refusalText(o.status, o.message), kErr);
        } else {
            pending[o.ticket] = {static_cast<int>(s), what};
            setNote(s, what + ": " + tr("sent"), kDim);
        }
    }
    static bool slintlanelRefusal(int status) { return statusIsRefusal(status); }
    void refuseHere(Sec s, const std::string& what, const std::string& why) { ++refused; setNote(s, what + ": " + why, kErr); }
    // sends a built JSON text, or says why it cannot be built
    void sendBuilt(Sec s, const std::string& what, const Built& b, std::uint32_t op, long long i0 = 0, long long i1 = 0, std::uint32_t radio = 0) {
        if (!b.ok) { refuseHere(s, what, b.err); return; }
        Cmd c = cmd(op, i0, b.text);
        c.i1 = i1;
        send(s, what, c, radio);
    }

    void onAnswer(const FoxCommandResult& r) {
        const auto it = pending.find(r.ticket);
        if (it == pending.end()) { return; }
        const Sec s = static_cast<Sec>(it->second.first);
        const std::string what = it->second.second;
        pending.erase(it);
        ++answered;
        const std::string msg = trim(r.message);
        if (r.status == FOXAPI_OK) { setNote(s, what + ": " + tr("done") + (msg.empty() ? std::string() : " (" + msg + ")"), kGood); }
        else if (r.status == FOXAPI_NO_CHANGE) { setNote(s, what + ": " + refusalText(r.status, msg), kDim); }
        else { ++refused; setNote(s, what + ": " + refusalText(r.status, msg), kErr); }
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
    // a draft choice or tick: kept as typed until a key sends it
    void choiceD(std::vector<Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int def) {
        choice(v, id, label, opts, pv(id, def), [this, id](int i) { draft[id] = std::to_string(i); });
    }
    void tickD(std::vector<Row>& v, const std::string& id, const std::string& label, bool def, const std::string& tip = std::string()) {
        tick(v, id, label, bv(id, def), [this, id](bool on) { draft[id] = on ? "1" : "0"; }, tip);
    }
    void factLines(std::vector<Row>& v, const std::string& factText, std::size_t maxLines) {
        for (const auto& kv : flattenJson(factText, maxLines)) { v.push_back(rReadout(kv.first, kv.second)); }
    }
    void endNote(std::vector<Row>& v, Sec s) { if (!noteText[s].empty()) { v.push_back(rNote(noteText[s], noteTone[s])); } }
    // wire-c's settings: key "<key>\x1F<value>", read back as fact.wirec.<key>
    void setting(Sec s, const std::string& what, const std::string& k, const std::string& value) { send(s, what, cmd(FOXAPP_OP_WIREC_SETTING, 0, k + "\x1F" + value)); }
    std::string wirec(const std::string& k) { return fact("wirec." + k); }
    void settingTick(std::vector<Row>& v, Sec s, const std::string& id, const std::string& label, const std::string& k, bool def, const std::string& tip = std::string()) {
        const std::string f = wirec(k);
        const bool on = f.empty() ? def : f == "1";
        tick(v, id, label, on, [this, s, label, k](bool o) { setting(s, label, k, o ? "1" : "0"); }, tip);
    }
    static std::string mhz(double hz) { return numberText(std::round(hz / 1e3) / 1e3) + " MHz"; }

    // ================================================================ A. TUNING AND MEMORIES
    void buildTuning(std::vector<Row>& v) {
        const int n = sel();
        const std::string rn = std::to_string(n);
        v.push_back(rHeading(tr("Frequency history")));
        key(v, "t.hist-back", tr("Back"), [this] { send(kTuning, tr("Back"), cmd(FOXAPP_OP_HISTORY_STEP, 0)); }, true, tr("Retunes to the previous frequency of this radio. At the end of the history nothing changes."));
        key(v, "t.hist-fwd", tr("Forward"), [this] { send(kTuning, tr("Forward"), cmd(FOXAPP_OP_HISTORY_STEP, 1)); }, true, tr("Retunes to the next frequency of this radio's history."));
        const std::string hz = fact("tuned." + rn + ".hz");
        if (!hz.empty()) { v.push_back(rReadout(tr("Tuned"), mhz(std::atof(hz.c_str())) + "  " + fact("tuned." + rn + ".mode"))); }

        v.push_back(rHeading(tr("Channel plan")));
        static const char* const planNames[8] = {"PMR446, analogue", "PMR446, digital 12.5 kHz", "PMR446, digital 6.25 kHz", "CB, 40 channels (CEPT)", "FRS (US)", "GMRS (US)", "MURS (US)", "Marine VHF (ITU)"};
        std::vector<std::string> names;
        for (const char* p : planNames) { names.push_back(tr(p)); }
        int cur = -1;
        const std::string plan = fact("channel." + rn + ".plan");
        for (std::size_t i = 0; i < channelPlanIds().size(); ++i) { if (channelPlanIds()[i] == plan) { cur = static_cast<int>(i); } }
        choice(v, "t.plan", tr("Channel plan"), names, cur, [this](int i) { if (i >= 0 && i < 8) { send(kTuning, tr("Channel plan"), cmd(FOXAPP_OP_CHANNEL, 0, channelPlanIds()[static_cast<std::size_t>(i)])); } },
               tr("Choosing a plan tunes the radio to its first channel."));
        const std::string label = fact("channel." + rn + ".label");
        if (!label.empty()) { v.push_back(rReadout(tr("Channel"), label + "  " + mhz(std::atof(fact("channel." + rn + ".freqHz").c_str())))); }
        const std::string note = fact("channel." + rn + ".note");
        if (!note.empty()) { v.push_back(rNote(note)); }
        key(v, "t.chan-next", tr("Next channel"), [this] { send(kTuning, tr("Next channel"), cmd(FOXAPP_OP_CHANNEL, 1)); });
        key(v, "t.chan-prev", tr("Previous channel"), [this] { send(kTuning, tr("Previous channel"), cmd(FOXAPP_OP_CHANNEL, 2)); });
        field(v, "t.chan-go", tr("Go to channel"), "", tr("The channel's label, for example 16 or G7; press Enter, then the key"));
        key(v, "t.chan-goto", tr("Go to channel now"), [this] { send(kTuning, tr("Go to channel"), cmd(FOXAPP_OP_CHANNEL, 3, trim(dv("t.chan-go")))); }, !trim(dv("t.chan-go")).empty());
        key(v, "t.chan-leave", tr("Leave channel mode"), [this] { send(kTuning, tr("Leave channel mode"), cmd(FOXAPP_OP_CHANNEL, 4)); });

        v.push_back(rHeading(tr("Markers")));
        field(v, "t.mk-path", tr("Markers file"), "", tr("The full path of the file to write or read (CSV or JSON). An export never overwrites a file that exists."));
        key(v, "t.mk-csv", tr("Export CSV..."), [this] { send(kTuning, tr("Export markers"), cmd(FOXAPP_OP_MARKER_EXPORT, 0, trim(dv("t.mk-path")))); }, !trim(dv("t.mk-path")).empty());
        key(v, "t.mk-json", tr("Export JSON..."), [this] { send(kTuning, tr("Export markers"), cmd(FOXAPP_OP_MARKER_EXPORT, 1, trim(dv("t.mk-path")))); }, !trim(dv("t.mk-path")).empty());
        field(v, "t.mk-reach", tr("Merge markers within (Hz)"), "0", tr("A marker this close to one in the list is merged with it; 0 = the same frequency only"));
        key(v, "t.mk-import", tr("Import..."), [this] {
            double r = 0; std::string e;
            if (!parseNumber(dv("t.mk-reach", "0"), 0.0, 1e9, r, e)) { refuseHere(kTuning, tr("Import markers"), e); return; }
            Cmd c = cmd(FOXAPP_OP_MARKER_IMPORT, 0, trim(dv("t.mk-path"))); c.n0 = r; send(kTuning, tr("Import markers"), c);
        }, !trim(dv("t.mk-path")).empty());
        const std::string mio = fact("markerio.last");
        if (!mio.empty()) { v.push_back(rReadout(tr("Last markers file"), mio)); }

        v.push_back(rHeading(tr("Bookmark notes and fields")));
        field(v, "t.bm-id", tr("Bookmark number"), "", tr("The number of the bookmark the note, tags and fields below belong to"));
        field(v, "t.bm-note", tr("Bookmark note"), "", tr("Up to 2000 bytes"));
        field(v, "t.bm-tags", tr("Bookmark tags"), "", tr("Up to 16 tags of 32 characters, separated by commas"));
        const auto bmId = [this](long long& id, Sec s, const std::string& what) { std::string e; if (!parseInteger(dv("t.bm-id"), 1, 2000000000LL, id, e)) { refuseHere(s, what, e); return false; } return true; };
        key(v, "t.bm-save", tr("Save note and tags"), [this, bmId] {
            long long id = 0; if (!bmId(id, kTuning, tr("Save note"))) { return; }
            sendBuilt(kTuning, tr("Save note"), bookmarkNoteJson(dv("t.bm-note"), dv("t.bm-tags")), FOXAPP_OP_BOOKMARK_NOTE, id, 0);
        });
        key(v, "t.bm-read", tr("Read the note"), [this, bmId] { long long id = 0; if (!bmId(id, kTuning, tr("Read note"))) { return; } Cmd c = cmd(FOXAPP_OP_BOOKMARK_NOTE, id); c.i1 = 1; send(kTuning, tr("Read note"), c); });
        key(v, "t.bm-clear", tr("Remove the note"), [this, bmId] { long long id = 0; if (!bmId(id, kTuning, tr("Remove note"))) { return; } send(kTuning, tr("Remove note"), cmd(FOXAPP_OP_BOOKMARK_NOTE, id, "")); });
        const std::string lastNote = fact("bookmark.note.last");
        if (!lastNote.empty()) { v.push_back(rReadout(tr("Note of bookmark") + " " + fact("bookmark.note.lastId"), jsonString(lastNote, "note"))); }
        field(v, "t.bm-squelch", tr("Bookmark squelch"), "", tr("dB, -200 to 20; empty = none. Applied when the bookmark is tuned."));
        field(v, "t.bm-threshold", tr("Bookmark threshold"), "", tr("dB, -200 to 50; empty = none. The scanner's own level for this entry."));
        field(v, "t.bm-offset", tr("Bookmark offset"), "", tr("kHz; stored with the bookmark and not applied"));
        field(v, "t.bm-tone", tr("Bookmark tone"), "", tr("CTCSS tone in Hz; empty = none. Applied when the bookmark is tuned."));
        key(v, "t.bm-fsave", tr("Save the fields"), [this, bmId] {
            long long id = 0; if (!bmId(id, kTuning, tr("Save fields"))) { return; }
            sendBuilt(kTuning, tr("Save fields"), bookmarkFieldsJson(dv("t.bm-squelch"), dv("t.bm-threshold"), dv("t.bm-offset"), dv("t.bm-tone")), FOXAPP_OP_BOOKMARK_FIELDS, id, 0);
        });
        key(v, "t.bm-fread", tr("Read the fields"), [this, bmId] { long long id = 0; if (!bmId(id, kTuning, tr("Read fields"))) { return; } Cmd c = cmd(FOXAPP_OP_BOOKMARK_FIELDS, id); c.i1 = 1; send(kTuning, tr("Read fields"), c); });
        key(v, "t.bm-fclear", tr("Remove the fields"), [this, bmId] { long long id = 0; if (!bmId(id, kTuning, tr("Remove fields"))) { return; } Cmd c = cmd(FOXAPP_OP_BOOKMARK_FIELDS, id, "{}"); send(kTuning, tr("Remove fields"), c); });
        const std::string lastF = fact("bookmark.fields.last");
        if (!lastF.empty()) { v.push_back(rNote(tr("Fields of bookmark") + " " + fact("bookmark.fields.lastId") + ": " + lastF)); }
        field(v, "t.bm-find", tr("Search notes and tags"), "", tr("A word in a note or a tag"));
        key(v, "t.bm-findtext", tr("Find"), [this] { send(kTuning, tr("Find"), cmd(FOXAPP_OP_BOOKMARK_FIND, 0, trim(dv("t.bm-find")))); }, !trim(dv("t.bm-find")).empty());
        key(v, "t.bm-findtag", tr("Find by tag"), [this] { send(kTuning, tr("Find by tag"), cmd(FOXAPP_OP_BOOKMARK_FIND, 1, trim(dv("t.bm-find")))); }, !trim(dv("t.bm-find")).empty());
        std::string found = fact("bookmark.find");
        if (!found.empty()) { for (auto& c : found) { if (c == '\n') { c = ' '; } } v.push_back(rReadout(tr("Bookmarks found"), found)); }

        v.push_back(rHeading(tr("Favourites")));
        field(v, "t.fav-name", tr("Favourite's name"), "", tr("1 to 64 characters"));
        key(v, "t.fav-save", tr("Save view as favourite"), [this] { sendBuilt(kTuning, tr("Save favourite"), favouriteSaveJson(dv("t.fav-name")), FOXAPP_OP_FAVOURITE_SAVE); }, !trim(dv("t.fav-name")).empty(),
            tr("Keeps the radio's frequency, mode, bandwidth and noise settings under that name."));
        key(v, "t.fav-recall", tr("Recall"), [this] { send(kTuning, tr("Recall"), cmd(FOXAPP_OP_FAVOURITE_RECALL, 0, trim(dv("t.fav-name")))); }, !trim(dv("t.fav-name")).empty(),
            tr("Retunes the radio and sets the mode, bandwidth and noise settings. The picture's own numbers (centre, span, levels) are not applied by this window yet."));
        field(v, "t.fav-new", tr("New name"), "");
        key(v, "t.fav-rename", tr("Rename"), [this] { send(kTuning, tr("Rename"), cmd(FOXAPP_OP_FAVOURITE_EDIT, 1, trim(dv("t.fav-name")) + "\t" + trim(dv("t.fav-new")))); }, !trim(dv("t.fav-name")).empty() && !trim(dv("t.fav-new")).empty());
        key(v, "t.fav-remove", tr("Remove"), [this] { send(kTuning, tr("Remove favourite"), cmd(FOXAPP_OP_FAVOURITE_EDIT, 0, trim(dv("t.fav-name")))); }, !trim(dv("t.fav-name")).empty());
        const std::string favCount = fact("favourite.count");
        if (!favCount.empty()) {
            std::string names = fact("favourite.names");
            for (auto& c : names) { if (c == '\n') { c = ';'; } }
            v.push_back(rReadout(tr("Favourites kept"), favCount + (names.empty() ? std::string() : ": " + names.substr(0, 200))));
        }

        v.push_back(rHeading(tr("Bookmark packs")));
        field(v, "t.pack-path", tr("Pack file"), "", tr("The full path of the pack to write or read"));
        field(v, "t.pack-name", tr("Pack name"), "");
        field(v, "t.pack-desc", tr("Pack description"), "");
        key(v, "t.pack-export", tr("Export pack..."), [this] { sendBuilt(kTuning, tr("Export pack"), packExportJson(dv("t.pack-path"), dv("t.pack-name"), dv("t.pack-desc")), FOXAPP_OP_PACK_EXPORT); }, !trim(dv("t.pack-path")).empty());
        key(v, "t.pack-import", tr("Import pack..."), [this] { send(kTuning, tr("Import pack"), cmd(FOXAPP_OP_PACK_IMPORT, 0, trim(dv("t.pack-path")))); }, !trim(dv("t.pack-path")).empty());
        const std::string pk = fact("pack.last");
        if (!pk.empty()) { v.push_back(rReadout(tr("Last pack"), pk)); }

        v.push_back(rHeading(tr("Band memory")));
        const std::string bmOn = fact("bandmem.on"), bmFields = fact("bandmem.fields");
        const auto sendBand = [this](bool on, bool vol, bool agc) { sendBuilt(kTuning, tr("Remember per band"), bandMemoryJson(on, vol, agc), FOXAPP_OP_BAND_MEMORY); };
        const bool vol = bmFields.find("volume") != std::string::npos, agc = bmFields.find("agc") != std::string::npos;
        tick(v, "t.band-on", tr("Remember per band"), bmOn == "1", [=](bool on) { sendBand(on, vol, agc); }, tr("Off by default: what you set on a band comes back when you enter it again."));
        tick(v, "t.band-vol", tr("Volume"), vol, [=](bool on) { sendBand(bmOn == "1", on, agc); });
        tick(v, "t.band-agc", tr("AGC"), agc, [=](bool on) { sendBand(bmOn == "1", vol, on); });
        key(v, "t.band-clear", tr("Forget every band"), [this] { send(kTuning, tr("Forget every band"), cmd(FOXAPP_OP_BAND_MEMORY, 0, "{\"clear\":true}")); });
        const std::string bands = fact("bandmem.bands");
        if (!bands.empty()) { v.push_back(rReadout(tr("Bands remembered"), bands)); }
        v.push_back(rNote(tr("The spectrum's limits and zoom, the attenuator and the antenna are not remembered by this window yet.")));

        v.push_back(rHeading(tr("Keys")));
        static const char* const acts[6] = {"bandwidth_up", "bandwidth_down", "cycle_mode", "centre_signal", "next_bookmark", "previous_bookmark"};
        const std::string labels[6] = {tr("Bandwidth up"), tr("Bandwidth down"), tr("Cycle mode"), tr("Centre on the signal"), tr("Next bookmark"), tr("Previous bookmark")};
        for (int i = 0; i < 6; ++i) {
            const std::string a = acts[i], l = labels[i];
            key(v, "t.act-" + a, l, [this, a, l] { send(kTuning, l, cmd(FOXAPP_OP_KEY_ACTION, 0, a)); });
        }
        endNote(v, kTuning);
    }

    // ================================================================ A. SCANNING
    void buildScan(std::vector<Row>& v) {
        v.push_back(rNote(tr("These are the engine's own scan plan, trigger and thresholds. The Scanner block under TOOLS keeps its own list scan, lock-outs, priority list and mute.")));
        v.push_back(rHeading(tr("Scan plan")));
        field(v, "s.ranges", tr("Ranges"), "", tr("Up to 16, separated by ; each: start MHz, stop MHz, step kHz, mode, bandwidth kHz (for example 144 146 12.5 NFM 12.5)"));
        choiceD(v, "s.dir", tr("Direction"), {tr("Forward"), tr("Backward")}, 0);
        key(v, "s.plan-set", tr("Set the plan"), [this] { planSend(0, tr("Set the plan")); }, !trim(dv("s.ranges")).empty());
        key(v, "s.plan-start", tr("Start the plan scan"), [this] { send(kScan, tr("Start the plan scan"), cmd(FOXAPP_OP_SCAN_PLAN, 1)); });
        key(v, "s.plan-clear", tr("Clear the plan"), [this] { send(kScan, tr("Clear the plan"), cmd(FOXAPP_OP_SCAN_PLAN, 2)); });
        key(v, "s.back", tr("Scan back"), [this] { send(kScan, tr("Scan back"), cmd(FOXAPP_OP_SCAN_BACK)); }, true, tr("During a plan scan: steps back to the point before the one the scan is on."));
        key(v, "s.save-hit", tr("Save hit as bookmark"), [this] { send(kScan, tr("Save hit as bookmark"), cmd(FOXAPP_OP_SCAN_SAVE_HIT)); }, true, tr("Adds the frequency the scan is on to the group Scan hits."));
        const std::string active = fact("scan.plan.active");
        if (!active.empty()) { v.push_back(rReadout(tr("Plan scan"), active == "1" ? tr("running") : tr("not running"))); }

        v.push_back(rHeading(tr("Trigger")));
        choiceD(v, "s.trig-mode", tr("Trigger"), {tr("Fixed level"), tr("Above the noise floor")}, 1);
        field(v, "s.trig-level", tr("Trigger level (dB)"), "-70", tr("dB, the fixed level (-160 to 20)"));
        field(v, "s.trig-margin", tr("Trigger margin (dB)"), "8", tr("dB above the noise floor (0 to 60)"));
        field(v, "s.trig-hyst", tr("Trigger hysteresis (dB)"), "2", tr("dB below the trigger at which it releases (0 to 30)"));
        field(v, "s.trig-rise", tr("Trigger floor rise (dB per reading)"), "0.1", tr("How fast the remembered noise floor may rise, dB per reading (0 to 10)"));
        key(v, "s.trig-on", tr("Use this trigger"), [this] {
            const Built b = scanTriggerJson(pv("s.trig-mode", 1), dv("s.trig-level", "-70"), dv("s.trig-margin", "8"), dv("s.trig-hyst", "2"), dv("s.trig-rise", "0.1"));
            sendBuilt(kScan, tr("Use this trigger"), b, FOXAPP_OP_SCAN_TRIGGER, 1);
        });
        key(v, "s.trig-off", tr("Back to the squelch"), [this] { send(kScan, tr("Back to the squelch"), cmd(FOXAPP_OP_SCAN_TRIGGER, 0)); });
        const std::string trigOn = fact("scan.trigger.on");
        if (!trigOn.empty()) { v.push_back(rReadout(tr("Trigger"), trigOn == "1" ? tr("on") : tr("off (the squelch decides)"))); }

        v.push_back(rHeading(tr("Per-entry thresholds")));
        field(v, "s.thr", tr("Thresholds"), "", tr("Up to 1000, separated by ; each: MHz and dB (for example 145.5 -90)"));
        key(v, "s.thr-set", tr("Set the thresholds"), [this] { thresholdsSend(); }, !trim(dv("s.thr")).empty());
        key(v, "s.thr-clear", tr("Clear the thresholds"), [this] { send(kScan, tr("Clear the thresholds"), cmd(FOXAPP_OP_SCANNER_EXTRAS, 4, "")); });

        v.push_back(rHeading(tr("Scan the whole span without retuning")));
        tickD(v, "s.fft-on", tr("Scan whole span without retuning"), false, tr("Looks for signals across the span the radio already shows, five times a second at most. Off by default."));
        field(v, "s.fft-start", tr("Span scan start (MHz)"), "88");
        field(v, "s.fft-stop", tr("Span scan stop (MHz)"), "108");
        field(v, "s.fft-step", tr("Span scan step (kHz)"), "100");
        choiceD(v, "s.fft-mode", tr("Hit level"), {tr("Above the floor"), tr("Fixed level")}, 0);
        field(v, "s.fft-margin", tr("Span scan margin (dB)"), "8");
        field(v, "s.fft-fixed", tr("Span scan fixed level (dB)"), "-70");
        tickD(v, "s.fft-merge", tr("Merge neighbouring hits"), true);
        key(v, "s.fft-apply", tr("Apply the span scan"), [this] {
            const Built b = fftScanJson(bv("s.fft-on", false), dv("s.fft-start", "88"), dv("s.fft-stop", "108"), dv("s.fft-step", "100"), pv("s.fft-mode", 0), dv("s.fft-margin", "8"), dv("s.fft-fixed", "-70"), bv("s.fft-merge", true));
            sendBuilt(kScan, tr("Span scan"), b, FOXAPP_OP_FFT_SCAN, 0);
        });
        key(v, "s.fft-read", tr("Read the hits"), [this] { send(kScan, tr("Read the hits"), cmd(FOXAPP_OP_FFT_SCAN, 1, "")); });
        factLines(v, fact("fftscan." + std::to_string(sel()) + ".result"), 12);

        v.push_back(rHeading(tr("Station database")));
        field(v, "s.db-path", tr("Station database"), "", tr("The full path of your own CSV file; it is read, never copied or changed"));
        field(v, "s.db-freq", tr("Frequency column"), "frequency", tr("The name of the column that holds the frequency"));
        choiceD(v, "s.db-unit", tr("Unit"), {"Hz", "kHz", "MHz"}, 2);
        field(v, "s.db-cols", tr("Columns"), "", tr("Up to 16 column names to show, separated by commas"));
        choiceD(v, "s.db-delim", tr("Delimiter"), {tr("Comma"), tr("Semicolon"), tr("Tab")}, 0);
        key(v, "s.db-load", tr("Load the database"), [this] { sendBuilt(kScan, tr("Load the database"), overlayLoadJson(dv("s.db-path"), pv("s.db-unit", 2), dv("s.db-freq", "frequency"), dv("s.db-cols"), pv("s.db-delim", 0)), FOXAPP_OP_OVERLAY_LOAD); });
        key(v, "s.db-remove", tr("Remove the database"), [this] { send(kScan, tr("Remove the database"), cmd(FOXAPP_OP_OVERLAY_LOAD, 0, "")); });
        const std::string rows = fact("overlay.rows");
        if (!rows.empty()) { v.push_back(rReadout(tr("Rows loaded"), rows + "  (" + fact("overlay.skipped") + " " + tr("skipped") + ")")); }
        field(v, "s.db-look", tr("Look up (MHz)"), "", tr("The frequency to look up in the database"));
        key(v, "s.db-lookup", tr("Look up"), [this] {
            double f = 0; std::string e;
            if (!parseNumber(dv("s.db-look"), 0.000001, 1e7, f, e)) { refuseHere(kScan, tr("Look up"), e); return; }
            Cmd c = cmd(FOXAPP_OP_OVERLAY_LOOKUP, 3); c.n0 = f * 1e6; c.n1 = 0; send(kScan, tr("Look up"), c);
        }, !trim(dv("s.db-look")).empty());
        factLines(v, fact("overlay.lookup"), 8);
        endNote(v, kScan);
    }
    void planSend(int mode, const std::string& what) {
        const Built t = scanTriggerJson(pv("s.trig-mode", 1), dv("s.trig-level", "-70"), dv("s.trig-margin", "8"), dv("s.trig-hyst", "2"), dv("s.trig-rise", "0.1"));
        if (!t.ok) { refuseHere(kScan, what, t.err); return; }
        sendBuilt(kScan, what, scanPlanJson(dv("s.ranges"), pv("s.dir", 0), t.text), FOXAPP_OP_SCAN_PLAN, mode);
    }
    void thresholdsSend() {
        std::string out;
        std::vector<std::string> items;
        splitList(dv("s.thr"), 1000, 80, items);
        for (const auto& it : items) {
            std::string a, b;
            const std::size_t sp = it.find_first_of(" \t");
            if (sp == std::string::npos) { refuseHere(kScan, tr("Set the thresholds"), tr("write MHz and dB for each entry")); return; }
            a = it.substr(0, sp); b = trim(it.substr(sp));
            double f = 0, d = 0; std::string e;
            if (!parseNumber(a, 0.000001, 1e7, f, e) || !parseNumber(b, -200.0, 50.0, d, e)) { refuseHere(kScan, tr("Set the thresholds"), e); return; }
            char line[96]; std::snprintf(line, sizeof(line), "%.0f\t%.2f\n", f * 1e6, d);
            out += line;
        }
        send(kScan, tr("Set the thresholds"), cmd(FOXAPP_OP_SCANNER_EXTRAS, 4, out));
    }

    // ================================================================ A. RECORDING AND FILES
    void buildRecord(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Daily folders")));
        const std::string daily = fact("record.daily");
        const int style = static_cast<int>(jsonNumber(daily, "style", 0));
        const bool utc = jsonBool(daily, "utc", true);
        choice(v, "r.daily-style", tr("Daily folders"), {tr("None"), "2026-11-11", "2026/11/11", "2026-11/11", "20261111"}, style, [this, utc](int i) { sendBuilt(kRecord, tr("Daily folders"), recordDailyJson(i, utc), FOXAPP_OP_SET_RECORD_DAILY); },
               tr("A take goes into a folder for the day it starts (the second line shows the shape of the name)."));
        tick(v, "r.daily-utc", tr("Use UTC"), utc, [this, style](bool on) { sendBuilt(kRecord, tr("Use UTC"), recordDailyJson(style, on), FOXAPP_OP_SET_RECORD_DAILY); }, tr("Off = the computer's local date."));

        v.push_back(rHeading(tr("While recording")));
        const bool lockOn = fact("record.lock") == "1", gainLock = fact("record.lock.gain") == "1";
        tick(v, "r.lock", tr("Lock settings while recording"), lockOn, [this, gainLock](bool on) { Cmd c = cmd(FOXAPP_OP_RECORD_LOCK, on ? 1 : 0); c.i1 = gainLock ? 1 : 0; send(kRecord, tr("Lock settings while recording"), c); },
             tr("Off by default: a source change during an I/Q take ends the take. On: the change is refused instead, with the reason."));
        tick(v, "r.lock-gain", tr("Also lock gain"), gainLock, [this, lockOn](bool on) { Cmd c = cmd(FOXAPP_OP_RECORD_LOCK, lockOn ? 1 : 0); c.i1 = on ? 1 : 0; send(kRecord, tr("Also lock gain"), c); });
        const std::string frozen = fact("record.lock.frozen." + std::to_string(sel()));
        if (!frozen.empty()) {
            std::string f = frozen;
            for (auto& c : f) { if (c == '\n') { c = ','; } }
            v.push_back(rReadout(tr("Locked now"), f));
            const std::string why = fact("record.lock.reason");
            if (!why.empty()) { v.push_back(rNote(why, kWarn)); }
        }
        const std::string cont = fact("record.container");
        choice(v, "r.container", tr("Audio file format"), {"WAV", "FLAC"}, cont == "flac" ? 1 : 0, [this](int i) { Cmd c = cmd(FOXAPP_OP_RECORD_CONTAINER, i); c.i1 = fact("record.container.keep") == "1" ? 1 : 0; send(kRecord, tr("Audio file format"), c); },
               tr("FLAC is made from the finished WAV after the take, for audio takes only."));
        tick(v, "r.keepwav", tr("Keep the WAV"), fact("record.container.keep") == "1", [this, cont](bool on) { Cmd c = cmd(FOXAPP_OP_RECORD_CONTAINER, cont == "flac" ? 1 : 0); c.i1 = on ? 1 : 0; send(kRecord, tr("Keep the WAV"), c); });

        v.push_back(rHeading(tr("Analyser job")));
        const std::string js = fact("job.state");
        v.push_back(rReadout(tr("Job"), js.empty() ? tr("none") : js + (fact("job.name").empty() ? std::string() : ": " + fact("job.name"))));
        if (js == "running") { v.push_back(rBar(tr("Progress"), static_cast<float>(std::atof(fact("job.progress").c_str())), fact("job.progress"))); }
        const std::string jm = fact("job.message");
        if (!jm.empty()) { v.push_back(rNote(jm, js == "failed" ? kErr : kDim)); }
        factLines(v, fact("job.result"), 10);
        key(v, "r.job-cancel", tr("Cancel"), [this] { send(kRecord, tr("Cancel the job"), cmd(FOXAPP_OP_JOB_CANCEL)); }, js == "running", tr("Asks the running job to stop at its next block."));
        field(v, "r.in", tr("Recording"), "", tr("The full path of the recording to work on"));
        field(v, "r.out", tr("New file"), "", tr("The full path of the file to write; an existing file is never overwritten"));
        field(v, "r.t0", tr("From (s)"), "0");
        field(v, "r.t1", tr("To (s)"), "");
        field(v, "r.lo", tr("Lowest frequency (Hz)"), "");
        field(v, "r.hi", tr("Highest frequency (Hz)"), "");
        choiceD(v, "r.mode", tr("Write as"), {tr("I/Q"), "AM", "FM", "USB", "LSB"}, 0);
        choiceD(v, "r.fmt", tr("Sample format"), {"float32", "pcm16", "cf32"}, 0);
        key(v, "r.export", tr("Export selection"), [this] { sendBuilt(kRecord, tr("Export selection"), exportBoxJson(dv("r.in"), dv("r.out"), dv("r.t0", "0"), dv("r.t1"), dv("r.lo"), dv("r.hi"), pv("r.mode", 0), pv("r.fmt", 0)), FOXAPP_OP_EXPORT_BOX); },
            !trim(dv("r.in")).empty(), tr("The part of the recording inside the time and frequency box, mixed to baseband, as I/Q or sound."));
        key(v, "r.hash", tr("Hash the recording"), [this] { json j = {{"action", "hash"}, {"recording", trim(dv("r.in"))}}; send(kRecord, tr("Hash the recording"), cmd(FOXAPP_OP_PROJECT, 0, j.dump(-1, ' ', false, json::error_handler_t::replace))); }, !trim(dv("r.in")).empty());
        field(v, "r.proj", tr("Project file"), "", tr("The full path of an analyser project (.fxproj)"));
        key(v, "r.verify", tr("Check the project's recording"), [this] {
            json j = {{"action", "verify"}, {"project", trim(dv("r.proj"))}};
            if (!trim(dv("r.in")).empty()) { j["recording"] = trim(dv("r.in")); }
            send(kRecord, tr("Check the project"), cmd(FOXAPP_OP_PROJECT, 0, j.dump(-1, ' ', false, json::error_handler_t::replace)));
        }, !trim(dv("r.proj")).empty());

        v.push_back(rHeading(tr("Attachments")));
        field(v, "r.att-folder", tr("Record's folder"), "", tr("An existing folder: its attachments.json lists the screenshots, clips and notes kept with the record"));
        field(v, "r.att-path", tr("File to add"), "", tr("A screenshot or a clip; the file's type is decided by its first bytes"));
        field(v, "r.att-title", tr("Title"), "");
        field(v, "r.att-text", tr("Note text"), "", tr("A note instead of a file"));
        field(v, "r.att-id", tr("Attachment number"), "");
        const auto attach = [this](const char* action, const char* kind) {
            json j = {{"folder", trim(dv("r.att-folder"))}, {"action", action}};
            if (std::strcmp(action, "add") == 0) { j["kind"] = kind; if (std::strcmp(kind, "note") == 0) { j["text"] = dv("r.att-text"); } else { j["path"] = trim(dv("r.att-path")); } j["title"] = dv("r.att-title"); }
            if (std::strcmp(action, "remove") == 0) { long long id = 0; std::string e; if (!parseInteger(dv("r.att-id"), 1, 1000000, id, e)) { refuseHere(kRecord, tr("Remove"), e); return; } j["id"] = id; }
            send(kRecord, tr("Attachments"), cmd(FOXAPP_OP_ATTACH, 0, j.dump(-1, ' ', false, json::error_handler_t::replace)));
        };
        const bool folderOk = !trim(dv("r.att-folder")).empty();
        key(v, "r.att-shot", tr("Add a screenshot"), [attach] { attach("add", "screenshot"); }, folderOk && !trim(dv("r.att-path")).empty());
        key(v, "r.att-clip", tr("Add a clip"), [attach] { attach("add", "clip"); }, folderOk && !trim(dv("r.att-path")).empty());
        key(v, "r.att-note", tr("Add a note"), [attach] { attach("add", "note"); }, folderOk && !trim(dv("r.att-text")).empty());
        key(v, "r.att-list", tr("List"), [attach] { attach("list", ""); }, folderOk);
        key(v, "r.att-check", tr("Check the files"), [attach] { attach("verify", ""); }, folderOk);
        key(v, "r.att-remove", tr("Remove"), [attach] { attach("remove", ""); }, folderOk && !trim(dv("r.att-id")).empty());

        v.push_back(rHeading(tr("SigMF annotation")));
        field(v, "r.sg-meta", tr("SigMF metadata file"), "", tr("The full path of a finished recording's .sigmf-meta"));
        field(v, "r.sg-rate", tr("Sample rate (Hz)"), "");
        field(v, "r.sg-label", tr("Label"), "");
        field(v, "r.sg-comment", tr("Comment"), "");
        key(v, "r.sg-add", tr("Save as SigMF annotation"), [this] {
            double a = 0, b = 0, lo = 0, hi = 0, rate = 0; std::string e;
            if (!parseNumber(dv("r.t0", "0"), 0.0, 1e7, a, e) || !parseNumber(dv("r.t1"), 0.0, 1e7, b, e) || !parseNumber(dv("r.lo"), 0.0, 1e13, lo, e) || !parseNumber(dv("r.hi"), 0.0, 1e13, hi, e) || !parseNumber(dv("r.sg-rate"), 1.0, 1e10, rate, e)) { refuseHere(kRecord, tr("Save as SigMF annotation"), e + " (" + tr("the box above and the sample rate") + ")"); return; }
            json j = {{"meta", trim(dv("r.sg-meta"))}, {"action", "add"}, {"startSec", a}, {"endSec", b}, {"loHz", lo}, {"hiHz", hi}, {"sampleRateHz", rate}, {"totalSamples", 0}, {"label", dv("r.sg-label")}, {"comment", dv("r.sg-comment")}};
            send(kRecord, tr("Save as SigMF annotation"), cmd(FOXAPP_OP_SIGMF_ANNOTATE, 0, j.dump(-1, ' ', false, json::error_handler_t::replace)));
        }, !trim(dv("r.sg-meta")).empty());
        key(v, "r.sg-list", tr("List the annotations"), [this] { json j = {{"meta", trim(dv("r.sg-meta"))}, {"action", "list"}}; send(kRecord, tr("List the annotations"), cmd(FOXAPP_OP_SIGMF_ANNOTATE, 0, j.dump(-1, ' ', false, json::error_handler_t::replace))); }, !trim(dv("r.sg-meta")).empty());
        endNote(v, kRecord);
    }
    static Row rBar(const std::string& l, float lv, const std::string& val) { return slk::rBar(l, lv, val); }

    // ================================================================ B. MEASUREMENT
    void buildMeasure(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Field strength")));
        field(v, "m.af-path", tr("Antenna factor table"), "", tr("A CSV file: frequency, dB per metre, one point per line. The full path."));
        field(v, "m.cable-path", tr("Cable loss table"), "", tr("A CSV file: frequency, dB, one point per line. The full path."));
        choiceD(v, "m.unit", tr("Frequency unit of the tables"), {"Hz", "kHz", "MHz", "GHz"}, 2);
        const auto loadTable = [this](int which, const std::string& pathId, const std::string& what) {
            std::string text, e;
            if (!readWholeFile(dv(pathId), 1u << 20, text, e)) { refuseHere(kMeasure, what, e); return; }
            Cmd c = cmd(FOXAPP_OP_FIELD_TABLE, which, text); c.i1 = pv("m.unit", 2); send(kMeasure, what, c);
        };
        key(v, "m.af-load", tr("Antenna factor table..."), [loadTable] { loadTable(0, "m.af-path", tr("Antenna factor table")); }, !trim(dv("m.af-path")).empty());
        key(v, "m.cable-load", tr("Cable loss table..."), [loadTable] { loadTable(1, "m.cable-path", tr("Cable loss table")); }, !trim(dv("m.cable-path")).empty());
        key(v, "m.af-clear", tr("Clear the antenna factor table"), [this] { Cmd c = cmd(FOXAPP_OP_FIELD_TABLE, 0, ""); send(kMeasure, tr("Clear the antenna factor table"), c); });
        key(v, "m.cable-clear", tr("Clear the cable loss table"), [this] { Cmd c = cmd(FOXAPP_OP_FIELD_TABLE, 1, ""); send(kMeasure, tr("Clear the cable loss table"), c); });
        const std::string af = fact("field.afPoints"), cl = fact("field.cablePoints");
        if (!af.empty() || !cl.empty()) { v.push_back(rReadout(tr("Table points"), (af.empty() ? "0" : af) + " / " + (cl.empty() ? "0" : cl))); }
        field(v, "m.preamp", tr("Preamp gain"), "0", tr("dB, -100 to 100"));
        field(v, "m.ohm", tr("Input ohm"), "50", tr("1 to 1000"));
        tick(v, "m.fshow", tr("Show"), fact("field.show") == "1", [this](bool on) {
            double g = 0, o = 0; std::string e;
            if (!parseNumber(dv("m.preamp", "0"), -100.0, 100.0, g, e) || !parseNumber(dv("m.ohm", "50"), 1.0, 1000.0, o, e)) { refuseHere(kMeasure, tr("Field strength"), e); return; }
            Cmd c = cmd(FOXAPP_OP_FIELD_CONFIG, on ? 1 : 0); c.n0 = g; c.n1 = o; send(kMeasure, tr("Field strength"), c);
        });
        key(v, "m.fread", tr("Read dBuV/m now"), [this] { send(kMeasure, tr("Read dBuV/m"), cmd(FOXAPP_OP_FIELD_READ)); });
        const std::string fr = selFact("field.readout");
        if (!fr.empty()) { v.push_back(rReadout("dBuV/m", fr)); }

        v.push_back(rHeading(tr("Signal measurements")));
        field(v, "m.dur", tr("Length (s)"), "0.25", tr("0.001 to 10: how much of the newest signal to look at"));
        field(v, "m.ago", tr("Ends (s before now)"), "0", tr("0 to 60"));
        static const struct { std::uint32_t op; const char* label; const char* name; } meas[9] = {
            {FOXAPP_OP_SIGMEAS_BANDWIDTH, "Occupied bandwidth", "bandwidth"}, {FOXAPP_OP_SIGMEAS_MODULATION, "FM deviation and AM depth", "modulation"}, {FOXAPP_OP_SIGMEAS_SYMBOLS, "Symbols", "symbols"},
            {FOXAPP_OP_SIGMEAS_VSTIME, "Amplitude, frequency and phase over time", "vstime"}, {FOXAPP_OP_SIGMEAS_BURSTS, "Bursts", "bursts"}, {FOXAPP_OP_SIGMEAS_HOPCHIRP, "Hops and chirps", "hopchirp"},
            {FOXAPP_OP_SIGMEAS_AUDIO, "SINAD, THD, SFDR and ENOB", "audio"}, {FOXAPP_OP_SIGMEAS_DRIFT, "Drift", "drift"}, {FOXAPP_OP_SIGMEAS_HISTOGRAM, "Histogram and CCDF", "histogram"}};
        for (const auto& m : meas) {
            const std::string name = m.name, label = tr(m.label);
            const std::uint32_t op = m.op;
            key(v, "m.meas-" + name, label, [this, op, name, label] { lastMeasure = name; sendBuilt(kMeasure, label, sigmeasJson(dv("m.dur", "0.25"), dv("m.ago", "0")), op); });
        }
        field(v, "m.pd-dwell", tr("Samples per antenna"), "100", tr("8 to 1000000"));
        key(v, "m.pd", tr("Pseudo-Doppler bearing"), [this] { lastMeasure = "pseudodoppler"; sendBuilt(kMeasure, tr("Pseudo-Doppler bearing"), pseudoDopplerJson(dv("m.pd-dwell", "100"), dv("m.dur", "0.25")), FOXAPP_OP_PSEUDO_DOPPLER); }, true,
            tr("Estimates a bearing from I/Q already captured with a switched four-antenna array; FoxSDR does not drive the antenna switch."));
        if (!lastMeasure.empty()) {
            const std::string r = selFact(("sigmeas." + lastMeasure).c_str());
            v.push_back(rNote(tr("Last measurement") + ": " + lastMeasure));
            if (!r.empty()) {
                const std::string reason = jsonString(r, "reason");
                if (!reason.empty()) { v.push_back(rNote(reason, kWarn)); }
                factLines(v, r, 24);
            }
        }

        v.push_back(rHeading(tr("Bit view")));
        static const struct { std::uint32_t op; const char* label; const char* name; bool params; } bits[8] = {
            {FOXAPP_OP_BITS_KEEP, "Keep a burst", "keep", true}, {FOXAPP_OP_BITS_LIST, "List kept bursts", "list", false}, {FOXAPP_OP_BITS_REDECODE, "Decode again", "redecode", false},
            {FOXAPP_OP_BITS_SWEEP, "Try every line code", "sweep", false}, {FOXAPP_OP_BITS_FIND_CRC, "Find the CRC", "findcrc", false}, {FOXAPP_OP_BITS_FIND_SUM, "Find the checksum", "findsum", false},
            {FOXAPP_OP_BITS_INFER, "Infer the fields", "infer", false}, {FOXAPP_OP_BITS_DIFF, "Compare two frames", "diff", false}};
        field(v, "m.bit-ab", tr("Frames to compare"), "1 2", tr("Two frame numbers from 1, for example 1 2"));
        for (const auto& b : bits) {
            const std::string name = b.name, label = tr(b.label);
            const std::uint32_t op = b.op;
            const bool params = b.params;
            key(v, "m.bit-" + name, label, [this, op, name, label, params] {
                lastBits = name;
                if (params) { sendBuilt(kMeasure, label, sigmeasJson(dv("m.dur", "0.25"), dv("m.ago", "0")), op); return; }
                if (name == "diff") {
                    long long a = 0, c = 0; std::string e;
                    const std::string t = trim(dv("m.bit-ab", "1 2"));
                    const std::size_t sp = t.find_first_of(" ,;");
                    if (sp == std::string::npos || !parseInteger(t.substr(0, sp), 1, 100000, a, e) || !parseInteger(trim(t.substr(sp + 1)), 1, 100000, c, e)) { refuseHere(kMeasure, label, e.empty() ? tr("write two frame numbers") : e); return; }
                    send(kMeasure, label, cmd(op, 0, json({{"a", a}, {"b", c}}).dump(-1, ' ', false, json::error_handler_t::replace)));
                    return;
                }
                send(kMeasure, label, cmd(op, 0, "{}"));
            });
        }
        field(v, "m.bit-file", tr("Decoder or code-table file"), "", tr("The full path of a text file: a decoder (foxsdr-bitspec 1) or a code table"));
        const auto fileOp = [this](std::uint32_t op, const char* nm, const std::string& what) {
            std::string text, e;
            if (!readWholeFile(dv("m.bit-file"), 1u << 20, text, e)) { refuseHere(kMeasure, what, e); return; }
            lastBits = nm;
            send(kMeasure, what, cmd(op, 0, text));
        };
        const bool bf = !trim(dv("m.bit-file")).empty();
        key(v, "m.bit-speccheck", tr("Check the decoder file"), [fileOp] { fileOp(FOXAPP_OP_BITS_SPEC_CHECK, "speccheck", tr("Check the decoder file")); }, bf);
        key(v, "m.bit-specrun", tr("Run the decoder file"), [fileOp] { fileOp(FOXAPP_OP_BITS_SPEC_RUN, "specrun", tr("Run the decoder file")); }, bf);
        key(v, "m.bit-tablecheck", tr("Check the code table file"), [fileOp] { fileOp(FOXAPP_OP_BITS_TABLE_CHECK, "tablecheck", tr("Check the code table file")); }, bf);
        if (!lastBits.empty()) {
            const std::string r = selFact(("bits." + lastBits).c_str());
            if (!r.empty()) { v.push_back(rNote(tr("Last bit view result") + ": " + lastBits)); factLines(v, r, 20); }
        }
        field(v, "m.fl-cap", tr("Frames kept"), "256", tr("1 to 4096"));
        key(v, "m.fl-keep", tr("Keep frames with CRC errors"), [this] { frameLogConfig(1); }, true, tr("The engine's frame log keeps frames in memory only, with a flag that says whether their CRC was right."));
        key(v, "m.fl-drop", tr("Drop frames with CRC errors"), [this] { frameLogConfig(0); });
        key(v, "m.fl-read", tr("Read the frame log"), [this] { Cmd c = cmd(FOXAPP_OP_FRAMELOG_READ, 1); send(kMeasure, tr("Read the frame log"), c); });
        const std::string fl = fact("framelog");
        if (!fl.empty()) { v.push_back(rReadout(tr("Frame log"), fl.size() > 160 ? fl.substr(0, 160) + "..." : fl)); }

        v.push_back(rHeading(tr("GPU spectrum")));
        const std::string gm = fact("gpu_fft.mode");
        choice(v, "m.gpu", tr("GPU spectrum"), {tr("CPU"), tr("Auto (the GPU from 65536 points)"), tr("GPU")}, gm.empty() ? 0 : std::atoi(gm.c_str()), [this](int i) { send(kMeasure, tr("GPU spectrum"), cmd(FOXAPP_OP_GPU_FFT_MODE, i)); },
               tr("CPU at every start. A failure of the GPU path is never an error: the CPU runs on."));
        const std::string gf = selFact("gpufft");
        if (!gf.empty()) {
            v.push_back(rReadout(tr("Running on"), jsonString(gf, "backend") + (jsonBool(gf, "engaged", false) ? "" : "  (" + tr("not engaged") + ")")));
            const std::string ge = jsonString(gf, "error");
            if (!ge.empty()) { v.push_back(rNote(ge, kWarn)); }
        }

        v.push_back(rHeading(tr("Limit alarm")));
        field(v, "m.mask-path", tr("Mask file for the alarm"), "", tr("A foxmask 1 file (the full path). The Spectrum tools section draws the same kind of file; this one sets the alarm."));
        key(v, "m.mask-load", tr("Load the mask for the alarm"), [this] {
            std::string text, e;
            if (!readWholeFile(dv("m.mask-path"), 1u << 20, text, e)) { refuseHere(kMeasure, tr("Load the mask for the alarm"), e); return; }
            send(kMeasure, tr("Load the mask for the alarm"), cmd(FOXAPP_OP_MASK_LOAD, 0, text));
        }, !trim(dv("m.mask-path")).empty());
        key(v, "m.mask-remove", tr("Remove the alarm's mask"), [this] { send(kMeasure, tr("Remove the alarm's mask"), cmd(FOXAPP_OP_MASK_LOAD, 0, "")); });
        const std::string mk = selFact("mask");
        tickD(v, "m.mask-on", tr("Alarm on"), jsonBool(mk, "enabled", false));
        field(v, "m.mask-raise", tr("Raise after (frames)"), "3", tr("1 to 100"));
        field(v, "m.mask-clear", tr("Clear after (frames)"), "5", tr("1 to 100"));
        tickD(v, "m.mask-capture", tr("Record I/Q on a breach"), false, tr("Starts the radio's I/Q recording when the alarm is raised. Off by default."));
        field(v, "m.mask-post", tr("Keep recording after (s)"), "2", tr("0 to 60"));
        field(v, "m.mask-min", tr("Least time between captures (s)"), "10", tr("0 to 3600"));
        key(v, "m.mask-apply", tr("Apply the alarm settings"), [this] { sendBuilt(kMeasure, tr("Limit alarm"), maskConfigJson(bv("m.mask-on", false), dv("m.mask-raise", "3"), dv("m.mask-clear", "5"), bv("m.mask-capture", false), dv("m.mask-post", "2"), dv("m.mask-min", "10")), FOXAPP_OP_MASK_CONFIG); });
        if (!mk.empty()) { factLines(v, mk, 10); }
        endNote(v, kMeasure);
    }
    void frameLogConfig(int keep) {
        long long cap = 0; std::string e;
        if (!parseInteger(dv("m.fl-cap", "256"), 1, 4096, cap, e)) { refuseHere(kMeasure, tr("Frame log"), e); return; }
        Cmd c = cmd(FOXAPP_OP_FRAMELOG_CONFIG, keep); c.i1 = cap; send(kMeasure, tr("Frame log"), c);
    }

    // ================================================================ B. SWEEP
    void buildSweep(std::vector<Row>& v) {
        v.push_back(rNote(tr("A spectrum analyser on a serial port or a HackRF in sweep mode: nothing opens until you press Open. Receive only.")));
        const int kind = pv("w.kind", 0);
        choiceD(v, "w.kind", tr("Kind"), {"tinySA", "tinySA Ultra", "HackRF", "RF Explorer"}, 0);
        if (kind != 2) { field(v, "w.port", tr("Sweep port"), "", tr("COM1 to COM256; nothing opens until you type it and press Open")); }
        if (kind == 3) { field(v, "w.baud", tr("Sweep baud rate"), "500000", tr("500000 is the maker's default")); }
        field(v, "w.start", tr("Sweep start (MHz)"), "");
        field(v, "w.stop", tr("Sweep stop (MHz)"), "");
        if (kind == 0 || kind == 1) { field(v, "w.points", tr("Sweep points"), "450", tr("2 to 1048576")); field(v, "w.interval", tr("Sweep interval (ms)"), "0", tr("0 to 60000")); }
        if (kind == 2) {
            field(v, "w.step", tr("Sweep step (MHz)"), "10", tr("1 to 20; whole megahertz only"));
            field(v, "w.lna", tr("LNA gain (dB)"), "16", tr("0 to 40"));
            field(v, "w.vga", tr("VGA gain (dB)"), "16", tr("0 to 62"));
            tickD(v, "w.amp", tr("Amplifier"), false);
        }
        if (kind == 3) {
            field(v, "w.top", tr("Top (dBm)"), "-10", tr("-500 to 0"));
            field(v, "w.bottom", tr("Bottom (dBm)"), "-120", tr("-500 to 0, below the top"));
            v.push_back(rNote(tr("Connect the RF Explorer by USB and choose its COM port (Windows shows it as a USB serial device; if no COM port appears, install the maker's USB driver). FoxSDR talks to it at 500000 baud, the maker's default, and uses the spectrum analyser models only.")));
        }
        key(v, "w.open", tr("Open the sweep device"), [this] {
            const Built b = sweepOpenJson(pv("w.kind", 0), dv("w.port"), dv("w.baud", "500000"), dv("w.start"), dv("w.stop"), dv("w.points", "450"), dv("w.interval", "0"), dv("w.step", "10"), dv("w.lna", "16"), dv("w.vga", "16"), bv("w.amp", false), dv("w.top", "-10"), dv("w.bottom", "-120"));
            sendBuilt(kSweep, tr("Open the sweep device"), b, FOXAPP_OP_SWEEP_OPEN);
        });
        key(v, "w.close", tr("Close the sweep device"), [this] { send(kSweep, tr("Close the sweep device"), cmd(FOXAPP_OP_SWEEP_CLOSE)); });
        const std::string st = fact("sweep.state");
        if (!st.empty()) {
            v.push_back(rReadout(tr("Sweep device"), jsonBool(st, "open", false) ? tr("open") + ", " + jsonString(st, "kind") : tr("closed")));
            if (jsonBool(st, "open", false)) { v.push_back(rReadout(tr("Sweeps"), numberText(jsonNumber(st, "sweeps", 0)) + "  " + tr("refused") + " " + numberText(jsonNumber(st, "refused", 0)) + "  " + numberText(jsonNumber(st, "points", 0)) + " " + tr("points"))); }
            const std::string le = jsonString(st, "last_error");
            if (!le.empty()) { v.push_back(rNote(le, kWarn)); }
        }

        v.push_back(rHeading(tr("Heat map and occupancy")));
        field(v, "w.hm-cols", tr("Heat map columns"), "", tr("1 to 65536"));
        field(v, "w.hm-rows", tr("Heat map rows"), "", tr("2 to 65536"));
        choiceD(v, "w.hm-mode", tr("Frequency mode"), {tr("Maximum"), tr("Mean")}, 0);
        tickD(v, "w.occ-on", tr("Occupancy"), false);
        field(v, "w.occ-start", tr("Occupancy start (MHz)"), "");
        field(v, "w.occ-stop", tr("Occupancy stop (MHz)"), "");
        field(v, "w.occ-step", tr("Channel step (kHz)"), "");
        field(v, "w.occ-margin", tr("Occupancy margin (dB)"), "10", tr("0 to 200"));
        field(v, "w.occ-pct", tr("Floor percentile"), "0.1", tr("0 to 0.5"));
        field(v, "w.occ-floor", tr("Fixed floor (dB)"), "-100", tr("Used when the floor is not taken from the percentile"));
        key(v, "w.cfg", tr("Apply heat map and occupancy"), [this] {
            const Built b = sweepHeatmapJson(dv("w.hm-cols"), dv("w.hm-rows"), pv("w.hm-mode", 0), bv("w.occ-on", false), dv("w.occ-start"), dv("w.occ-stop"), dv("w.occ-step"), dv("w.occ-margin", "10"), dv("w.occ-pct", "0.1"), dv("w.occ-floor", "-100"));
            sendBuilt(kSweep, tr("Heat map and occupancy"), b, FOXAPP_OP_SWEEP_CONFIG);
        });
        v.push_back(rHeading(tr("Read the sweep")));
        field(v, "w.rd-points", tr("Points to read"), "512", tr("2 to 4096"));
        field(v, "w.rd-x", tr("Position across the sweep"), "0.5", tr("0 (left) to 1 (right): the frequency there is reported"));
        tickD(v, "w.rd-occ", tr("Include the occupancy table"), false);
        key(v, "w.read", tr("Read the sweep"), [this] {
            long long p = 0; double x = 0; std::string e;
            if (!parseInteger(dv("w.rd-points", "512"), 2, 4096, p, e) || !parseNumber(dv("w.rd-x", "0.5"), 0.0, 1.0, x, e)) { refuseHere(kSweep, tr("Read the sweep"), e); return; }
            json j = {{"max_points", p}, {"x", x}, {"occupancy", bv("w.rd-occ", false)}};
            send(kSweep, tr("Read the sweep"), cmd(FOXAPP_OP_SWEEP_READ, 0, j.dump(-1, ' ', false, json::error_handler_t::replace)));
        });
        const std::string rd = fact("sweep.read");
        if (!rd.empty()) {
            const double click = jsonNumber(rd, "click_hz", 0);
            v.push_back(rReadout(tr("Range"), mhz(jsonNumber(rd, "start_hz", 0)) + " - " + mhz(jsonNumber(rd, "stop_hz", 0))));
            if (click > 0) { v.push_back(rReadout(tr("At that position"), mhz(click))); }
        }
        endNote(v, kSweep);
    }

    // ================================================================ B. NETWORK AND VENDOR RADIOS
    void buildSources(std::vector<Row>& v) {
        v.push_back(rNote(tr("Network and vendor radios that are not in the radio list. Nothing connects until you press one of the keys below. The address and arguments are described in the radio's own section of the manual.")));
        std::vector<std::string> names;
        for (const auto& n : sourceKindNames()) { names.push_back(tr(n.c_str())); }
        choiceD(v, "n.kind", tr("Source"), names, 0);
        field(v, "n.args", tr("Address and arguments"), "", tr("For a network radio the address comes first, for example 192.168.1.20 or host:port; the maker's own words are in the manual"));
        const Built b = openRadioId(pv("n.kind", 0), dv("n.args"));
        key(v, "n.add", tr("Add this as another radio"), [this] {
            const Built bb = openRadioId(pv("n.kind", 0), dv("n.args"));
            if (!bb.ok) { refuseHere(kSources, tr("Add this as another radio"), bb.err); return; }
            Cmd c = cmd(FOXAPI_OP_RADIO_ADD, 0, bb.text); c.i1 = 0; send(kSources, tr("Add this as another radio"), c);
        }, b.ok, tr("Opens a new radio on that source: it connects to the address you typed."));
        key(v, "n.use", tr("Use for the selected radio"), [this] {
            const Built bb = openRadioId(pv("n.kind", 0), dv("n.args"));
            if (!bb.ok) { refuseHere(kSources, tr("Use for the selected radio"), bb.err); return; }
            send(kSources, tr("Use for the selected radio"), cmd(FOXAPI_OP_SELECT_SOURCE, 0, bb.text));
        }, b.ok, tr("Switches the selected radio to that source: it connects to the address you typed."));
        if (pv("n.kind", 0) == 11) {
            v.push_back(rNote(tr("The SDR-IQ needs FTDI's D2XX driver (ftd2xx.dll). Install it from FTDI's web site, ftdichip.com (FoxSDR does not include it and does not download it), plug the SDR-IQ in, then choose it. The arguments are sdriq=<serial> (empty = the first one) and timeout=<ms>.")));
        }
        v.push_back(rNote(tr("Whether a radio's driver or program is installed is shown under Needed libraries.")));
        endNote(v, kSources);
    }

    // ================================================================ B. OUTPUTS, FOLLOW, TRACKS
    void buildOutputs(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Follow a master")));
        field(v, "o.follower", tr("Follower radio"), "2", tr("1 to 8"));
        field(v, "o.master", tr("Master radio"), "1", tr("1 to 8"));
        const auto fol = [this](long long& f, const std::string& what) { std::string e; if (!parseInteger(dv("o.follower", "2"), 1, 8, f, e)) { refuseHere(kOutputs, what, e); return false; } return true; };
        key(v, "o.follow", tr("Follow a master"), [this, fol] {
            long long f = 0, m = 0; std::string e;
            if (!fol(f, tr("Follow a master"))) { return; }
            if (!parseInteger(dv("o.master", "1"), 1, 8, m, e)) { refuseHere(kOutputs, tr("Follow a master"), e); return; }
            Cmd c = cmd(FOXAPP_OP_FOLLOW_SET, f); c.i1 = m; send(kOutputs, tr("Follow a master"), c);
        }, true, tr("The follower keeps the distance it has now from the master's frequency."));
        key(v, "o.follow-stop", tr("Stop following"), [this, fol] { long long f = 0; if (fol(f, tr("Stop following"))) { send(kOutputs, tr("Stop following"), cmd(FOXAPP_OP_FOLLOW_CLEAR, f)); } });
        key(v, "o.follow-lock", tr("Lock follower"), [this, fol] { long long f = 0; if (fol(f, tr("Lock follower"))) { Cmd c = cmd(FOXAPP_OP_FOLLOW_LOCK, f); c.i1 = 1; send(kOutputs, tr("Lock follower"), c); } }, true, tr("The follower stays where it is while its master moves."));
        key(v, "o.follow-unlock", tr("Unlock follower"), [this, fol] { long long f = 0; if (fol(f, tr("Unlock follower"))) { Cmd c = cmd(FOXAPP_OP_FOLLOW_LOCK, f); c.i1 = 0; send(kOutputs, tr("Unlock follower"), c); } });
        key(v, "o.follow-keep", tr("Keep this distance"), [this, fol] { long long f = 0; if (fol(f, tr("Keep this distance"))) { send(kOutputs, tr("Keep this distance"), cmd(FOXAPP_OP_FOLLOW_KEEP, f)); } }, true, tr("The distance the follower has now becomes the one it keeps."));
        {
            const json rows = json::parse(fact("matrix.rows"), nullptr, false);
            if (rows.is_array()) {
                for (const auto& r : rows) {
                    if (!r.is_object() || !r.contains("follows") || !r["follows"].is_number() || r["follows"].get<double>() <= 0) { continue; }
                    char t[160];
                    const std::string lockedWord = r.value("locked", false) ? ", " + tr("locked") : std::string();
                    std::snprintf(t, sizeof(t), "%s %d %s %d%s, %s %.0f Hz", tr("Radio").c_str(), static_cast<int>(r.value("radio", 0)), tr("follows radio").c_str(), static_cast<int>(r.value("follows", 0)), lockedWord.c_str(), tr("offset").c_str(), r.value("offset_hz", 0.0));
                    v.push_back(rReadout(tr("Linked"), t));
                    const std::string fn = r.value("follow_note", std::string());
                    if (!fn.empty()) { v.push_back(rNote(fn, kWarn)); }
                }
            }
        }

        v.push_back(rHeading(tr("Network I/Q out")));
        v.push_back(rNote(tr("Sends the selected radio's raw I/Q to a program on this computer. It listens on this computer only.")));
        choiceD(v, "o.iq-format", tr("I/Q format"), {tr("8-bit unsigned"), tr("16-bit signed"), tr("32-bit float")}, 0);
        tickD(v, "o.iq-header", tr("Put a header before the samples"), false);
        tickD(v, "o.iq-udp", tr("Use UDP instead of TCP"), false, tr("UDP sends to 127.0.0.1 only."));
        field(v, "o.iq-port", tr("I/Q output port"), "0", tr("0 lets Windows choose (TCP only); UDP needs 1 to 65535"));
        key(v, "o.iq-start", tr("Start the I/Q output"), [this] { sendBuilt(kOutputs, tr("Network I/Q out"), iqOutJson(pv("o.iq-format", 0), bv("o.iq-header", false), bv("o.iq-udp", false), dv("o.iq-port", "0")), FOXAPP_OP_IQ_OUT_START); });
        key(v, "o.iq-stop", tr("Stop the I/Q output"), [this] { send(kOutputs, tr("Stop the I/Q output"), cmd(FOXAPP_OP_IQ_OUT_STOP)); });
        factLines(v, selFact("iqout"), 9);

        v.push_back(rHeading(tr("SBS output")));
        v.push_back(rNote(tr("Aircraft positions heard by the plugins, as SBS text lines for a map program on this computer. No call sign, squawk or label is sent.")));
        field(v, "o.sbs-port", tr("SBS port"), "0", tr("0 lets Windows choose"));
        key(v, "o.sbs-start", tr("Start the SBS output"), [this] { sendBuilt(kOutputs, tr("SBS output"), sbsOutJson(dv("o.sbs-port", "0")), FOXAPP_OP_SBS_OUT_START); });
        key(v, "o.sbs-stop", tr("Stop the SBS output"), [this] { send(kOutputs, tr("Stop the SBS output"), cmd(FOXAPP_OP_SBS_OUT_STOP)); });
        factLines(v, fact("sbs.out"), 6);

        v.push_back(rHeading(tr("Track store")));
        v.push_back(rNote(tr("Keeps the positions of aircraft, vessels and stations in memory for a replay. Nothing is written to disk; every start begins empty and off.")));
        const std::string ts = fact("tracks.state");
        tick(v, "o.tr-on", tr("Keep tracks"), jsonBool(ts, "enabled", false), [this](bool on) { sendBuilt(kOutputs, tr("Track store"), tracksConfigJson(on, dv("o.tr-keep", "21600")), FOXAPP_OP_TRACKS_CONFIG); });
        field(v, "o.tr-keep", tr("Keep for (s)"), "21600", tr("60 to 2592000; six hours is 21600"));
        key(v, "o.tr-clear", tr("Clear the stored tracks"), [this] { send(kOutputs, tr("Clear the stored tracks"), cmd(FOXAPP_OP_TRACKS_CONFIG, 0, "{\"clear\":true}")); });
        key(v, "o.tr-range", tr("Read the time held"), [this] { send(kOutputs, tr("Read the time held"), cmd(FOXAPP_OP_TRACKS_READ, 0, "{\"range\":true}")); });
        factLines(v, ts, 8);
        const std::string trr = fact("tracks.read");
        if (!trr.empty()) { factLines(v, trr, 6); }

        v.push_back(rHeading(tr("Coverage grid")));
        v.push_back(rNote(tr("A heat map of where aircraft were heard from. It needs your own position, which you type here: nothing reads your GPS or the computer's location. Memory only.")));
        const std::string cs = fact("coverage.state");
        field(v, "o.cv-lat", tr("Your latitude"), "", tr("-90 to 90, degrees"));
        field(v, "o.cv-lon", tr("Your longitude"), "", tr("-180 to 180, degrees"));
        field(v, "o.cv-cell", tr("Cell size (degrees)"), "0.05", tr("0.001 to 1"));
        field(v, "o.cv-half", tr("Half life (s)"), "259200", tr("3600 to 31536000; three days is 259200"));
        tick(v, "o.cv-on", tr("Coverage grid"), jsonBool(cs, "enabled", false), [this](bool on) { sendBuilt(kOutputs, tr("Coverage grid"), coverageConfigJson(on, dv("o.cv-lat"), dv("o.cv-lon"), dv("o.cv-cell", "0.05"), dv("o.cv-half", "259200")), FOXAPP_OP_COVERAGE_CONFIG); });
        key(v, "o.cv-clear", tr("Clear the coverage grid"), [this] { send(kOutputs, tr("Clear the coverage grid"), cmd(FOXAPP_OP_COVERAGE_CONFIG, 0, "{\"clear\":true}")); });
        key(v, "o.cv-read", tr("Read the coverage"), [this] { send(kOutputs, tr("Read the coverage"), cmd(FOXAPP_OP_COVERAGE_READ, 0, "{}")); });
        factLines(v, cs, 8);
        endNote(v, kOutputs);
    }

    // ================================================================ C. NEEDED LIBRARIES
    void buildLibs(std::vector<Row>& v) {
        v.push_back(rNote(tr("Some radios need a driver, a program or a file that FoxSDR does not include or supply. Each is listed here with what needs it, where to get it and whether this computer has it. The check reads file headers only: nothing is loaded, downloaded, copied or opened.")));
        key(v, "l.check-all", tr("Check every row again"), [this] { send(kLibs, tr("Check again"), cmd(FOXAPP_OP_NEEDED_LIB_CHECK, 0, "{}")); });
        libs = parseNeeded(fact("needed.libs"));
        for (const auto& r : libs) {
            neededRows(r, dv("l.path-" + r.id), v);
            const std::string id = r.id;
            handlers["l.check-" + id] = [this, id](const std::string&) { sendBuilt(kLibs, tr("Check again"), neededCheckJson(id, ""), FOXAPP_OP_NEEDED_LIB_CHECK); };
            handlers["l.path-" + id] = [this, id](const std::string& t) { draft["l.path-" + id] = t; dirty = true; };
            handlers["l.checkfile-" + id] = [this, id](const std::string&) { sendBuilt(kLibs, tr("Check this file"), neededCheckJson(id, dv("l.path-" + id)), FOXAPP_OP_NEEDED_LIB_CHECK); };
        }
        if (libs.empty()) { v.push_back(rNote(tr("The engine has not published the list yet."), kWarn)); }
        endNote(v, kLibs);
    }

    // ================================================================ D. SYSTEM
    void buildSystem(std::vector<Row>& v) {
        v.push_back(rHeading(tr("Power")));
        settingTick(v, kSystem, "y.keep-awake", tr("Keep the PC awake while recording"), "sys.keep_awake", true, tr("Never the display: only the computer's sleep, and only while a recording or a scheduled capture runs."));
        settingTick(v, kSystem, "y.resume", tr("Look for radios again after the PC wakes"), "sys.resume_reopen", true);
        const std::string rc = fact("sys.resume.count");
        if (!rc.empty()) { v.push_back(rReadout(tr("Times the radios were looked for again"), rc)); }
        v.push_back(rHeading(tr("Plugin store")));
        const std::string so = wirec("store.sort");
        choice(v, "y.store-sort", tr("Order of the plugin store"), {tr("Catalogue order"), tr("Name"), tr("Last updated")}, so == "name" ? 1 : so == "updated" ? 2 : 0, [this](int i) { static const char* const w[3] = {"catalogue", "name", "updated"}; setting(kSystem, tr("Order of the plugin store"), "store.sort", w[std::clamp(i, 0, 2)]); },
               tr("Popular and best rated need a server's figures, which do not exist yet."));
        v.push_back(rHeading(tr("About")));
        const std::string lb = fact("sys.lastboot");
        v.push_back(lb.empty() ? rNote(tr("No record of the last start has been published.")) : rNote(lb));
        endNote(v, kSystem);
    }

    // ================================================================ D. WEB
    void buildWeb(std::vector<Row>& v) {
        settingTick(v, kWeb, "b.metrics", tr("Show the /metrics page to signed-in browsers"), "web.metrics", false, tr("Off by default: the page is as if it did not exist."));
        std::string origins = wirec("web.cors_origins");
        for (auto& c : origins) { if (c == '\n') { c = ';'; } }
        v.push_back(rField("b.origins", tr("Allowed web origins"), dv("b.origins", origins), tr("Pages from these origins may read this program's web pages. Separate them with a semicolon, for example https://example.com;http://localhost:3000. Empty = none.")));
        handlers["b.origins"] = [this](const std::string& t) {
            std::string val = t;
            for (auto& c : val) { if (c == ';' || c == ',') { c = '\n'; } }
            std::string out, line;
            std::vector<std::string> items;
            std::string cur;
            for (const char c : val) { if (c == '\n') { if (!trim(cur).empty()) { items.push_back(trim(cur)); } cur.clear(); } else { cur.push_back(c); } }
            if (!trim(cur).empty()) { items.push_back(trim(cur)); }
            for (const auto& it : items) { if (!out.empty()) { out += "\n"; } out += it; }
            draft["b.origins"] = t;
            setting(kWeb, tr("Allowed web origins"), "web.cors_origins", out);
        };
        v.push_back(rNote(tr("Cross-origin sign-in stays off: it needs HTTPS, which is the owner's decision.")));
        endNote(v, kWeb);
    }

    // ================================================================ D. HAM TOOLS
    void buildHam(std::vector<Row>& v) {
        v.push_back(rNote(tr("A log you keep by hand. FoxSDR is a receiver and never transmits. The log is the file qso-log.json beside your settings; nothing in it is sent anywhere.")));
        v.push_back(rHeading(tr("Contest header")));
        field(v, "h.contest", tr("Contest"), "", tr("For example CQ-WW-CW"));
        field(v, "h.hcall", tr("Call sign of the log"), "");
        field(v, "h.loc", tr("Location"), "");
        field(v, "h.name", tr("Name"), "");
        field(v, "h.addr", tr("Address lines"), "", tr("Separate the lines with a semicolon"));
        field(v, "h.ops", tr("Operators"), "", tr("Separated by commas"));
        field(v, "h.soap", tr("Soapbox"), "", tr("Separate the lines with a semicolon"));
        key(v, "h.hsave", tr("Save the contest header"), [this] { sendBuilt(kHam, tr("Contest header"), cabrilloHeaderJson(dv("h.contest"), dv("h.hcall"), dv("h.loc"), dv("h.name"), dv("h.addr"), dv("h.ops"), dv("h.soap")), FOXAPP_OP_CABRILLO_HEADER); });
        const std::string cc = fact("ham.cabrillo.contests");
        if (!cc.empty()) { std::string c = cc; for (auto& ch : c) { if (ch == '\n') { ch = ';'; } } v.push_back(rReadout(tr("Contests the engine can lay out"), c.size() > 200 ? c.substr(0, 200) : c)); }

        v.push_back(rHeading(tr("Log a QSO")));
        field(v, "h.q-time", tr("Time (unix seconds)"), "", tr("Empty = now"));
        field(v, "h.q-khz", tr("Frequency (kHz)"), "");
        field(v, "h.q-mode", tr("Mode"), "", tr("CW, SSB, FT8 ..."));
        field(v, "h.q-call", tr("Call sign worked"), "");
        field(v, "h.q-me", tr("Your call sign"), "");
        field(v, "h.q-sent", tr("Exchange sent"), "", tr("Values separated by commas, for example 599, 12"));
        field(v, "h.q-recv", tr("Exchange received"), "");
        key(v, "h.q-add", tr("Log a QSO"), [this] { sendBuilt(kHam, tr("Log a QSO"), qsoJson(dv("h.q-time"), dv("h.q-khz"), dv("h.q-mode"), dv("h.q-call"), dv("h.q-me"), dv("h.q-sent"), dv("h.q-recv")), FOXAPP_OP_QSO_ADD); });
        const std::string qc = fact("ham.qso.count");
        v.push_back(rReadout(tr("QSOs in the log"), qc.empty() ? "0" : qc));
        const std::string qe = fact("ham.error");
        if (!qe.empty()) { v.push_back(rNote(qe, kWarn)); }
        field(v, "h.q-del", tr("QSO number"), "", tr("From 1"));
        key(v, "h.q-remove", tr("Remove the QSO"), [this] { long long n = 0; std::string e; if (!parseInteger(dv("h.q-del"), 1, 20000, n, e)) { refuseHere(kHam, tr("Remove the QSO"), e); return; } send(kHam, tr("Remove the QSO"), cmd(FOXAPP_OP_QSO_REMOVE, n)); });
        const bool armed = armedClear == "h.q-clear";
        key(v, "h.q-clear", armed ? tr("Press again to clear the whole log") : tr("Clear the log"), [this] {
            if (armedClear != "h.q-clear") { armedClear = "h.q-clear"; setNote(kHam, tr("Clear the log: press the key again to erase every QSO. This cannot be undone."), kWarn); return; }
            armedClear.clear();
            send(kHam, tr("Clear the log"), cmd(FOXAPP_OP_QSO_CLEAR));
        }, true, tr("Erases every contact in the log. The key asks you to press it a second time."));
        field(v, "h.exp-path", tr("Export file"), "", tr("The full path of the file to write"));
        key(v, "h.exp-cab", tr("Export Cabrillo log..."), [this] { send(kHam, tr("Export Cabrillo log"), cmd(FOXAPP_OP_CABRILLO_EXPORT, 0, trim(dv("h.exp-path")))); }, !trim(dv("h.exp-path")).empty());
        key(v, "h.exp-adif", tr("Export ADIF log..."), [this] { send(kHam, tr("Export ADIF log"), cmd(FOXAPP_OP_ADIF_EXPORT, 0, trim(dv("h.exp-path")))); }, !trim(dv("h.exp-path")).empty());
        const std::string el = fact("ham.export.last");
        if (!el.empty()) { v.push_back(rNote(el)); }
        const std::string pr = fact("ham.cabrillo.problems");
        if (!pr.empty()) { v.push_back(rNote(pr, kWarn)); }

        v.push_back(rHeading(tr("Follow radio information")));
        v.push_back(rNote(tr("Shows the frequency, mode and split that DXLab Commander or N1MM+ broadcast on the network. It is only shown: it never retunes FoxSDR and never sends anything.")));
        settingTick(v, kHam, "h.ri-on", tr("Follow radio info from DXLab Commander or N1MM+"), "hamops.radioinfo.enabled", false);
        v.push_back(rField("h.ri-addr", tr("Listen on address"), dv("h.ri-addr", wirec("hamops.radioinfo.address")), tr("127.0.0.1 = this computer only")));
        handlers["h.ri-addr"] = [this](const std::string& t) { draft["h.ri-addr"] = t; setting(kHam, tr("Listen on address"), "hamops.radioinfo.address", trim(t)); };
        v.push_back(rField("h.ri-port", tr("Listen on port"), dv("h.ri-port", wirec("hamops.radioinfo.port")), tr("The port the logging program sends to; 0 = not set up, nothing is opened")));
        handlers["h.ri-port"] = [this](const std::string& t) { draft["h.ri-port"] = t; setting(kHam, tr("Listen on port"), "hamops.radioinfo.port", trim(t)); };
        settingTick(v, kHam, "h.ri-other", tr("Accept an address other than this computer"), "hamops.radioinfo.allow_other", false);
        for (int i = 1; i <= 4; ++i) {
            const std::string pfx = "ham.radioinfo." + std::to_string(i) + ".";
            const std::string app = fact(pfx + "app");
            if (app.empty()) { continue; }
            v.push_back(rReadout(app, mhz(std::atof(fact(pfx + "freq_hz").c_str())) + "  " + fact(pfx + "mode") + (fact(pfx + "transmitting") == "1" ? "  TX" : "")));
            const std::string er = fact(pfx + "error");
            if (!er.empty()) { v.push_back(rNote(er, kWarn)); }
        }

        v.push_back(rHeading(tr("Answer CAT commands")));
        v.push_back(rNote(tr("Lets a logging program read and set this program's frequency and mode as if it were a rig, on a TCP port. Every transmit command is refused.")));
        settingTick(v, kHam, "h.cat-on", tr("Answer CAT commands from a logging program"), "cat.emulation.enabled", false);
        const std::string dia = wirec("cat.emulation.dialect");
        choice(v, "h.cat-dialect", tr("CAT dialect"), {"Kenwood TS-480", "Yaesu FT-891"}, dia == "yaesu-ft891" ? 1 : 0, [this](int i) { setting(kHam, tr("CAT dialect"), "cat.emulation.dialect", i == 1 ? "yaesu-ft891" : "kenwood-ts480"); });
        v.push_back(rField("h.cat-port", tr("CAT port"), dv("h.cat-port", wirec("cat.emulation.port")), tr("The port the logging program connects to (not 4532, which the rigctl server uses); 0 = not set up")));
        handlers["h.cat-port"] = [this](const std::string& t) { draft["h.cat-port"] = t; setting(kHam, tr("CAT port"), "cat.emulation.port", trim(t)); };
        settingTick(v, kHam, "h.cat-all", tr("Listen on every address"), "cat.emulation.bind_all", false, tr("Off = this computer only. On lets other computers on the network connect."));
        const std::string run = fact("cat.emulation.running");
        v.push_back(rReadout(tr("CAT emulation"), run == "1" ? tr("running") + ", " + fact("cat.emulation.clients") + " " + tr("clients") : tr("not running")));
        const std::string ce = fact("cat.emulation.error");
        if (!ce.empty()) { v.push_back(rNote(ce, kWarn)); }
        endNote(v, kHam);
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
        auto& l = env.ui->global<LaneL>();
        switch (s) {
            case kTuning: l.set_rows_tuning(models[s]); break;
            case kScan: l.set_rows_scan(models[s]); break;
            case kRecord: l.set_rows_record(models[s]); break;
            case kMeasure: l.set_rows_measure(models[s]); break;
            case kSweep: l.set_rows_sweep(models[s]); break;
            case kSources: l.set_rows_sources(models[s]); break;
            case kOutputs: l.set_rows_outputs(models[s]); break;
            case kLibs: l.set_rows_libs(models[s]); break;
            case kSystem: l.set_rows_system(models[s]); break;
            case kWeb: l.set_rows_web(models[s]); break;
            case kHam: l.set_rows_ham(models[s]); break;
            default: break;
        }
    }

    void rebuildAll() {
        dirty = false;
        using Builder = void (Impl::*)(std::vector<Row>&);
        static const Builder builders[kSecCount] = {&Impl::buildTuning, &Impl::buildScan, &Impl::buildRecord, &Impl::buildMeasure, &Impl::buildSweep, &Impl::buildSources, &Impl::buildOutputs, &Impl::buildLibs, &Impl::buildSystem, &Impl::buildWeb, &Impl::buildHam};
        handlers.clear();
        for (int s = 0; s < kSecCount; ++s) {
            std::vector<Row> rows;
            (this->*builders[s])(rows);
            publishSection(static_cast<Sec>(s), rows);
        }
    }

    void act(const std::string& id, const std::string& text) {
        // a destructive key armed by its first press is disarmed by any other action
        if (!armedClear.empty() && armedClear != id) { armedClear.clear(); noteText[kHam].clear(); }
        const auto it = handlers.find(id);
        if (it == handlers.end()) { return; }
        const Handler h = it->second;
        h(text);
        rebuildAll();
    }
};

LaneLPublisher::LaneLPublisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
LaneLPublisher::~LaneLPublisher() = default;

void LaneLPublisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneL>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->rebuildAll();
}

void LaneLPublisher::tick(long tickNo) {
    // about twice a second the rows are read again from the facts (a few get_setting calls per section); at once after any action or answer
    if (p_->dirty || tickNo - p_->lastRebuildTick >= 15) {
        p_->lastRebuildTick = tickNo;
        p_->rebuildAll();
    }
}

void LaneLPublisher::onAnswer(const FoxCommandResult& r) { p_->onAnswer(r); }

std::string LaneLPublisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.sent); return std::string(); }
    if (what == "refused") { out = std::to_string(p.refused); return std::string(); }
    if (what == "answered") { out = std::to_string(p.answered); return std::string(); }
    if (what.rfind("note.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.noteText[s]; return std::string(); } }
        return "ll.note.<section>, not " + sec;
    }
    if (what.rfind("rows.", 0) == 0) {
        const std::string sec = what.substr(5);
        for (int s = 0; s < kSecCount; ++s) { if (sec == kSecName[s]) { out = p.lastCount[s]; return std::string(); } }
        return "ll.rows.<section>, not " + sec;
    }
    if (what.rfind("draft.", 0) == 0) { out = p.dv(what.substr(6)); return std::string(); }
    if (what.rfind("fact.", 0) == 0) { out = p.fact(what.substr(5)); return std::string(); }
    if (what.rfind("lib.", 0) == 0) {
        const std::string id = what.substr(4);
        for (const auto& r : p.libs) { if (r.id == id) { out = neededStateWords(r); return std::string(); } }
        return "no library " + id;
    }
    if (what == "libs") { out = std::to_string(p.libs.size()); return std::string(); }
    return "ll.<sent|refused|answered|note.<section>|rows.<section>|draft.<id>|fact.<name>|lib.<id>|libs>, not " + what;
}

}  // namespace foxsdr::slintlanel
