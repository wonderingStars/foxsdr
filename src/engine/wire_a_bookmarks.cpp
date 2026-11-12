// wire_a_bookmarks.cpp - WIRING LANE wire-a, the bookmark side: notes and tags (BOOKMARK_NOTE, BOOKMARK_FIND; fact.bookmark.*).
//
// NOTES AND TAGS (core/bookmark_notes.hpp, row 4): a note and up to 16 tags per bookmark, kept in bookmark_notes.json beside markers.json and keyed by (frequency, mode), so they
// survive the bookmark list being imported again. The bookmark list itself is untouched (core/bookmark.hpp is shared data). A bookmark that is retuned or whose mode is changed
// takes its note with it: once a pass, when the list's version moved, the keys the bookmarks stand under are compared with the last look and the model's rekey() follows the change (no
// edit path has to know the notes exist). The file is loaded when the per-user folder is known and written half a second after the last change on a worker (and at the exit drain).
// Notes are the user's own words: they go to the file and to LOCAL facts only, never to a log.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/bookmark.hpp"
#include "core/bookmark_pack.hpp"
#include "core/diag_log.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace t = cascade::core::tuning;

const cascade::core::Bookmark* WireA::bookmarkById(std::uint64_t id) const {
    const std::size_t i = e_.freqMgr_.indexOfId(id);
    if (i >= e_.freqMgr_.list().size()) { return nullptr; }
    return &e_.freqMgr_.list()[i];
}

void WireA::bookmarksLoadOnce() {
    if (bookmarksLoaded_ || dir().empty()) { return; }
    bookmarksLoaded_ = true;
    std::string text = readFile("bookmark_notes.json");
    if (!text.empty()) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        std::string err;
        if (j.is_discarded() || !notes_.fromJson(j, &err)) {
            notesError_ = j.is_discarded() ? "the notes file is not JSON" : err;
            notes_.clear();
            bump();
        }
    }
    text = readFile("bookmark_fields.json");
    if (!text.empty()) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_discarded() && j.is_object() && j.contains("fields") && j["fields"].is_array()) {
            for (const auto& e : j["fields"]) {
                if (!e.is_object() || !e.contains("hz") || !e["hz"].is_number_integer() || !e.contains("mode") || !e["mode"].is_string()) { continue; }
                t::BookmarkNotes::Key k;
                if (!t::BookmarkNotes::makeKey(static_cast<double>(e["hz"].get<std::int64_t>()), e["mode"].get<std::string>(), k)) { continue; }
                BmFields f;
                auto num = [&](const char* name, double& dst, double lo, double hi) {
                    if (e.contains(name) && e[name].is_number() && std::isfinite(e[name].get<double>()) && e[name].get<double>() >= lo && e[name].get<double>() <= hi) { dst = e[name].get<double>(); }
                };
                num("squelchDb", f.squelchDb, -200.0, 20.0);
                num("thresholdDb", f.thresholdDb, -200.0, 50.0);
                num("offsetKhz", f.offsetKhz, -1.0e6, 1.0e6);
                num("toneHz", f.toneHz, 0.001, 1000.0);
                if (!f.empty() && fields_.size() < 20000) { fields_[k] = f; }
            }
        }
    }
    text = readFile("favourites.json");
    if (!text.empty()) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        std::string err;
        if (j.is_discarded() || !favourites_.fromJson(j, &err)) {
            favouritesError_ = j.is_discarded() ? "the favourites file is not JSON" : err;
            favourites_ = t::FavouriteStore();
            bump();
        }
    }
}

void WireA::notesChanged() {
    notesDirty_ = true;
    notesDueS_ = e_.clockS() + 0.5;
    bump();
}

namespace {
t::BookmarkNotes::Key keyOf(const cascade::core::Bookmark& b, bool& ok) {
    t::BookmarkNotes::Key k;
    ok = t::BookmarkNotes::makeKey(b.freqHz, b.mode, k);
    return k;
}
}  // namespace

void WireA::bookmarksPass() {
    bookmarksLoadOnce();
    const unsigned v = e_.freqMgr_.version();
    if (v != bookmarksSeenVersion_) {
        bookmarksSeenVersion_ = v;
        std::map<std::uint64_t, t::BookmarkNotes::Key> now;
        std::set<t::BookmarkNotes::Key> present;
        for (const cascade::core::Bookmark& b : e_.freqMgr_.list()) {
            bool ok = false;
            const t::BookmarkNotes::Key k = keyOf(b, ok);
            if (!ok) { continue; }
            now[b.id] = k;
            present.insert(k);
        }
        // A bookmark whose key moved takes the record with it, unless another bookmark still stands under the old key.
        for (const auto& [id, k] : now) {
            const auto it = bookmarkKeys_.find(id);
            if (it == bookmarkKeys_.end() || it->second == k || present.count(it->second) != 0) { continue; }
            if (notes_.rekey(static_cast<double>(it->second.first), it->second.second, static_cast<double>(k.first), k.second)) { notesChanged(); }
            if (const auto f = fields_.find(it->second); f != fields_.end() && fields_.count(k) == 0) {
                fields_[k] = f->second;
                fields_.erase(f);
                fieldsDirty_ = true;
                fieldsDueS_ = e_.clockS() + 0.5;
                bump();
            }
        }
        bookmarkKeys_ = std::move(now);
    }
    if (favouritesDirty_ && e_.clockS() >= favouritesDueS_) {
        favouritesDirty_ = false;
        saveFile("favourites.json", favourites_.toJson().dump(1) + "\n");
    }
    if (fieldsDirty_ && e_.clockS() >= fieldsDueS_) {
        fieldsDirty_ = false;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [k, f] : fields_) {
            nlohmann::json o = {{"hz", k.first}, {"mode", k.second}};
            if (!std::isnan(f.squelchDb)) { o["squelchDb"] = f.squelchDb; }
            if (!std::isnan(f.thresholdDb)) { o["thresholdDb"] = f.thresholdDb; }
            if (!std::isnan(f.offsetKhz)) { o["offsetKhz"] = f.offsetKhz; }
            if (!std::isnan(f.toneHz)) { o["toneHz"] = f.toneHz; }
            arr.push_back(std::move(o));
        }
        saveFile("bookmark_fields.json", nlohmann::json{{"version", 1}, {"fields", arr}}.dump(1) + "\n");
    }
    if (notesDirty_ && e_.clockS() >= notesDueS_) {
        notesDirty_ = false;
        saveFile("bookmark_notes.json", notes_.toJson().dump(1) + "\n");
    }
}

WireA::Result WireA::bookmarkNoteOp(const FoxCommand& c, const std::string& text, Result res) {
    bookmarksLoadOnce();
    if (c.ival[0] <= 0) { return refused(res, FOXAPI_NOT_FOUND, "no bookmark with that id"); }
    const cascade::core::Bookmark* b = bookmarkById(static_cast<std::uint64_t>(c.ival[0]));
    if (b == nullptr) { return refused(res, FOXAPI_NOT_FOUND, "no bookmark with that id"); }
    if (c.ival[1] == 1) {
        const t::BookmarkNote* n = notes_.find(b->freqHz, b->mode);
        noteLastId_ = std::to_string(c.ival[0]);
        noteLast_ = n == nullptr ? std::string() : nlohmann::json{{"note", n->note}, {"tags", n->tags}}.dump();
        bump();
        return res;
    }
    if (c.ival[1] != 0) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[1] is 0 set or 1 read"); }
    std::string note;
    std::vector<std::string> tags;
    if (!text.empty()) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
        if (j.contains("note")) {
            if (!j["note"].is_string()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the note is not text"); }
            note = j["note"].get<std::string>();
        }
        if (j.contains("tags")) {
            if (!j["tags"].is_array()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the tags are not a list"); }
            for (const auto& x : j["tags"]) {
                if (!x.is_string()) { return refused(res, FOXAPI_BAD_ARGUMENT, "a tag is not text"); }
                tags.push_back(x.get<std::string>());
            }
        }
    }
    // The same as before changes nothing (and writes nothing).
    {
        t::BookmarkNotes probe = notes_;
        std::string err;
        if (!probe.set(b->freqHz, b->mode, note, tags, &err)) { return refused(res, FOXAPI_BAD_ARGUMENT, err); }
        if (probe.toJson() == notes_.toJson()) { return refused(res, FOXAPI_NO_CHANGE, "the note and tags are as they were"); }
    }
    std::string err;
    (void)notes_.set(b->freqHz, b->mode, note, tags, &err);
    notesChanged();
    return res;
}

WireA::Result WireA::bookmarkFindOp(const FoxCommand& c, const std::string& text, Result res) {
    bookmarksLoadOnce();
    if (c.ival[0] != 0 && c.ival[0] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 text or 1 tag"); }
    const std::vector<t::BookmarkNotes::Key> keys = c.ival[0] == 0 ? notes_.search(text) : notes_.byTag(text);
    const std::set<t::BookmarkNotes::Key> want(keys.begin(), keys.end());
    std::string ids;
    int n = 0;
    for (const cascade::core::Bookmark& b : e_.freqMgr_.list()) {
        bool ok = false;
        const t::BookmarkNotes::Key k = keyOf(b, ok);
        if (!ok || want.count(k) == 0) { continue; }
        if (!ids.empty()) { ids += '\n'; }
        ids += std::to_string(b.id);
        ++n;
    }
    findLast_ = ids;
    res.applied[0] = n;
    bump();
    return res;
}

void WireA::bookmarkFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("bookmark.notes.count", std::to_string(notes_.size()));
    fact("bookmark.notes.error", notesError_);
    fact("bookmark.note.last", noteLast_);
    fact("bookmark.note.lastId", noteLastId_);
    fact("bookmark.find", findLast_);
    fact("bookmark.fields.last", fieldsLast_);
    fact("bookmark.fields.lastId", fieldsLastId_);
    fact("bookmark.fields.count", std::to_string(fields_.size()));
    std::string names;
    for (const std::string& n : favourites_.names()) {
        if (!names.empty()) { names += '\n'; }
        names += n;
    }
    fact("favourite.names", names);
    fact("favourite.count", std::to_string(favourites_.size()));
    fact("favourite.error", favouritesError_);
    fact("favourite.recalled", favouriteRecalled_);
    fact("pack.last", packLast_);
}

// ---- favourites ---------------------------------------------------------------------------------------------------------------------------------

namespace {
// The model's DSP words against the engine's numbers (FOXAPI_AGC_*, kDeemphUs order); -1 for a word the engine has no such value for.
int agcIndex(const std::string& w) {
    if (w == "auto") { return 0; }
    if (w == "fast") { return 1; }
    if (w == "medium") { return 2; }
    if (w == "slow") { return 3; }
    if (w == "off") { return 4; }
    return -1;
}
const char* agcWord(int i) {
    static const char* const names[5] = {"auto", "fast", "medium", "slow", "off"};
    return i >= 0 && i < 5 ? names[i] : "auto";
}
int deemphIndexOf(const std::string& w) {
    if (w == "50us") { return 0; }
    if (w == "75us") { return 1; }
    if (w == "off") { return 2; }
    return -1;
}
const char* deemphWord(int i) {
    static const char* const names[3] = {"50us", "75us", "off"};
    return i >= 0 && i < 3 ? names[i] : "off";
}
double numberOr(const nlohmann::json& j, const char* key, double fallback, bool& ok) {
    if (!j.contains(key)) { return fallback; }
    if (!j[key].is_number()) { ok = false; return fallback; }
    return j[key].get<double>();
}
}  // namespace

void WireA::favouritesChanged() {
    favouritesDirty_ = true;
    favouritesDueS_ = e_.clockS() + 0.5;
    bump();
}

WireA::Result WireA::favouriteOp(const FoxCommand& c, const std::string& text, Result res) {
    bookmarksLoadOnce();
    if (c.op == FOXAPP_OP_FAVOURITE_EDIT) {
        if (c.ival[0] == 0) {
            if (!favourites_.remove(text)) { return refused(res, FOXAPI_NOT_FOUND, "there is no favourite with that name"); }
        } else if (c.ival[0] == 1) {
            const std::size_t tab = text.find('\t');
            if (tab == std::string::npos) { return refused(res, FOXAPI_BAD_ARGUMENT, "give the old and the new name, separated by a tab"); }
            const std::string from = text.substr(0, tab), to = text.substr(tab + 1);
            if (favourites_.find(from) == nullptr) { return refused(res, FOXAPI_NOT_FOUND, "there is no favourite with that name"); }
            if (!favourites_.rename(from, to)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the new name is empty, too long or already used"); }
        } else {
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 remove or 1 rename");
        }
        favouritesChanged();
        return res;
    }
    if (c.op == FOXAPP_OP_FAVOURITE_RECALL) {
        const t::FavouriteState* f = favourites_.find(text);
        if (f == nullptr) { return refused(res, FOXAPI_NOT_FOUND, "there is no favourite with that name"); }
        const t::FavouriteState fav = *f;
        // The tune first (the mode and the bandwidth come with it), then the DSP through the ops' own paths.
        tuneTo(fav.freqHz, fav.mode, fav.bandwidthHz);
        const int agc = agcIndex(fav.dsp.agc);
        if (agc >= 0) { (void)subCommand(FOXAPI_OP_SET_AGC, 0.0, agc); }
        (void)subCommand(FOXAPI_OP_SET_NR, fav.dsp.noiseReductionLevel, fav.dsp.noiseReduction ? 1 : 0, 1);
        (void)subCommand(FOXAPI_OP_SET_NB, 0.0, fav.dsp.noiseBlanker ? 1 : 0, 0);
        (void)subCommand(FOXAPI_OP_SET_NOTCH, 0.0, fav.dsp.notch ? 1 : 0, 0);
        (void)subCommand(FOXAPI_OP_SET_SQUELCH, fav.dsp.squelchDb, 0);
        const int de = deemphIndexOf(fav.dsp.deemphasis);
        if (de >= 0) { (void)subCommand(FOXAPI_OP_SET_DEEMPHASIS, 0.0, de); }
        favouriteRecalled_ = t::favouriteToJson(fav).dump();
        res.applied[0] = e_.currentAbsoluteHz();
        bump();
        return res;
    }
    // SAVE
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("name") || !j["name"].is_string()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is JSON with a name"); }
    t::FavouriteState f;
    f.name = j["name"].get<std::string>();
    bool ok = true;
    f.displayCentreHz = numberOr(j, "displayCentreHz", e_.currentAbsoluteHz(), ok);
    f.displaySpanHz = numberOr(j, "displaySpanHz", f.displaySpanHz, ok);
    f.spectrumTopDb = numberOr(j, "spectrumTopDb", f.spectrumTopDb, ok);
    f.spectrumRangeDb = numberOr(j, "spectrumRangeDb", f.spectrumRangeDb, ok);
    f.waterfallFloorDb = numberOr(j, "waterfallFloorDb", f.waterfallFloorDb, ok);
    f.waterfallCeilingDb = numberOr(j, "waterfallCeilingDb", f.waterfallCeilingDb, ok);
    if (!ok) { return refused(res, FOXAPI_BAD_ARGUMENT, "a picture value is not a number"); }
    Receiver& rxs = e_.rx();
    f.freqHz = e_.currentAbsoluteHz();
    f.mode = rxs.modeIndex_ >= 0 && rxs.modeIndex_ < 8 ? cascade::gui::kModeNames[rxs.modeIndex_] : "WFM";
    f.bandwidthHz = rxs.vfoBandwidthHz_;
    f.dsp.agc = agcWord(rxs.agcMode_);
    f.dsp.noiseReduction = rxs.nrEnabled_;
    f.dsp.noiseReductionLevel = std::max(0.0, std::min(1.0, static_cast<double>(rxs.nrStrength_)));
    f.dsp.noiseBlanker = rxs.nbEnabled_;
    f.dsp.notch = rxs.notchEnabled_;
    f.dsp.squelchDb = std::max(-120.0, std::min(0.0, static_cast<double>(rxs.squelchDb_)));
    f.dsp.deemphasis = deemphWord(rxs.deemphIndex_);
    const bool existed = favourites_.find(f.name) != nullptr;
    std::string err;
    if (!favourites_.set(f, &err)) {
        const bool full = !existed && favourites_.size() >= t::FavouriteStore::kMaxEntries;
        return refused(res, full ? FOXAPI_LIMIT : FOXAPI_BAD_ARGUMENT, err);
    }
    favouritesChanged();
    return res;
}

// ---- packs --------------------------------------------------------------------------------------------------------------------------------------

WireA::Result WireA::packOp(const FoxCommand& c, const std::string& text, Result res) {
    bookmarksLoadOnce();
    std::string why;
    if (c.op == FOXAPP_OP_PACK_EXPORT) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("path") || !j["path"].is_string()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is JSON with a path"); }
        const std::string path = j["path"].get<std::string>();
        if (!fullPath(path, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
        const std::string name = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : std::string("FoxSDR pack");
        const std::string description = j.contains("description") && j["description"].is_string() ? j["description"].get<std::string>() : std::string();
        const std::vector<cascade::core::Bookmark>& list = e_.freqMgr_.list();
        if (list.empty() && favourites_.size() == 0) { return refused(res, FOXAPI_NO_CHANGE, "there are no bookmarks or favourites to export"); }
        std::vector<t::FavouriteState> favs;
        for (const std::string& n : favourites_.names()) {
            if (const t::FavouriteState* f = favourites_.find(n)) { favs.push_back(*f); }
        }
        int skipped = 0;
        const std::string body = t::exportPack(name, description, list, &notes_, favs, &skipped);
        if (body.empty() || body.size() > t::kPackMaxBytes) { return refused(res, FOXAPI_FAILED, "the pack could not be made (the name or description is not valid, or it is too big)"); }
        if (!writeNewFile(path, body, why)) {
            packLast_ = "not written";
            bump();
            return refused(res, FOXAPI_FAILED, why);
        }
        packLast_ = "exported " + std::to_string(static_cast<int>(list.size()) - skipped);
        res.applied[0] = static_cast<double>(list.size()) - skipped;
        res.applied[1] = skipped;
        bump();
        return res;
    }
    // PACK_IMPORT
    if (!fullPath(text, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
    std::string body;
    if (!readUserFile(text, t::kPackMaxBytes, body, why)) { packLast_ = "not read"; bump(); return refused(res, FOXAPI_FAILED, why); }
    t::Pack pack;
    std::string err;
    if (!t::importPack(body, pack, err)) {
        packLast_ = "refused";
        bump();
        return refused(res, FOXAPI_BAD_ARGUMENT, "the pack was refused: " + err);
    }
    const t::PackImportPlan plan = t::planImport(pack, e_.freqMgr_.list());
    int added = 0;
    bool full = false;
    for (const t::PackBookmark& pb : plan.toAdd) {
        cascade::core::Bookmark b = pb.bookmark;
        b.id = 0;
        if (e_.freqMgr_.add(b) < 0) { full = true; break; }   // (add() never refuses today; the branch keeps the LIMIT answer if it ever does)
        ++added;
        if (!pb.note.note.empty() || !pb.note.tags.empty()) {
            std::string e2;
            if (notes_.find(b.freqHz, b.mode) == nullptr && notes_.set(b.freqHz, b.mode, pb.note.note, pb.note.tags, &e2)) { notesChanged(); }
        }
    }
    for (const t::FavouriteState& f : pack.favourites) {
        if (favourites_.find(f.name) != nullptr) { continue; }
        if (favourites_.set(f)) { favouritesChanged(); }
    }
    packLast_ = "added " + std::to_string(added) + ", " + std::to_string(plan.duplicates) + " already there";
    res.applied[0] = added;
    res.applied[1] = plan.duplicates;
    bump();
    if (full) { return refused(res, FOXAPI_LIMIT, "the bookmark list is full; what fitted was added"); }
    return res;
}

// ---- bookmark-carried fields ------------------------------------------------------------------------------------------------------------------------

const WireA::BmFields* WireA::fieldsFor(double hz, const std::string& mode) const {
    t::BookmarkNotes::Key k;
    if (!t::BookmarkNotes::makeKey(hz, mode, k)) { return nullptr; }
    const auto it = fields_.find(k);
    return it == fields_.end() ? nullptr : &it->second;
}

WireA::Result WireA::bookmarkFieldsOp(const FoxCommand& c, const std::string& text, Result res) {
    bookmarksLoadOnce();
    if (c.ival[0] <= 0) { return refused(res, FOXAPI_NOT_FOUND, "no bookmark with that id"); }
    const cascade::core::Bookmark* b = bookmarkById(static_cast<std::uint64_t>(c.ival[0]));
    if (b == nullptr) { return refused(res, FOXAPI_NOT_FOUND, "no bookmark with that id"); }
    t::BookmarkNotes::Key k;
    if (!t::BookmarkNotes::makeKey(b->freqHz, b->mode, k)) { return refused(res, FOXAPI_BAD_ARGUMENT, "the bookmark has no usable frequency or mode"); }
    if (c.ival[1] == 1) {
        fieldsLastId_ = std::to_string(c.ival[0]);
        const auto it = fields_.find(k);
        nlohmann::json o = nlohmann::json::object();
        if (it != fields_.end()) {
            if (!std::isnan(it->second.squelchDb)) { o["squelchDb"] = it->second.squelchDb; }
            if (!std::isnan(it->second.thresholdDb)) { o["thresholdDb"] = it->second.thresholdDb; }
            if (!std::isnan(it->second.offsetKhz)) { o["offsetKhz"] = it->second.offsetKhz; }
            if (!std::isnan(it->second.toneHz)) { o["toneHz"] = it->second.toneHz; }
        }
        fieldsLast_ = it == fields_.end() ? std::string() : o.dump();
        bump();
        return res;
    }
    if (c.ival[1] != 0) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[1] is 0 set or 1 read"); }
    BmFields f;
    if (!text.empty()) {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
        std::string bad;
        auto num = [&](const char* name, double& dst, double lo, double hi) {
            if (!j.contains(name) || j[name].is_null()) { return; }
            if (!j[name].is_number() || !std::isfinite(j[name].get<double>()) || j[name].get<double>() < lo || j[name].get<double>() > hi) { bad = std::string("the field ") + name + " is not valid"; return; }
            dst = j[name].get<double>();
        };
        num("squelchDb", f.squelchDb, -200.0, 20.0);
        num("thresholdDb", f.thresholdDb, -200.0, 50.0);
        num("offsetKhz", f.offsetKhz, -1.0e6, 1.0e6);
        num("toneHz", f.toneHz, 0.001, 1000.0);
        if (!bad.empty()) { return refused(res, FOXAPI_BAD_ARGUMENT, bad); }
    }
    const auto it = fields_.find(k);
    auto same = [](double a, double b2) { return (std::isnan(a) && std::isnan(b2)) || a == b2; };
    const bool unchanged = it == fields_.end() ? f.empty() : (same(it->second.squelchDb, f.squelchDb) && same(it->second.thresholdDb, f.thresholdDb) && same(it->second.offsetKhz, f.offsetKhz) && same(it->second.toneHz, f.toneHz));
    if (unchanged) { return refused(res, FOXAPI_NO_CHANGE, "the bookmark's values are as they were"); }
    if (f.empty()) {
        fields_.erase(k);
    } else {
        if (it == fields_.end() && fields_.size() >= 20000) { return refused(res, FOXAPI_LIMIT, "too many bookmarks carry values"); }
        fields_[k] = f;
    }
    fieldsDirty_ = true;
    fieldsDueS_ = e_.clockS() + 0.5;
    bump();
    return res;
}

void WireA::afterBookmarkTune(const cascade::core::Bookmark& b) {
    bookmarksLoadOnce();
    if (e_.scanner_.active()) { return; }   // a scan would change the squelch at every step
    const BmFields* f = fieldsFor(b.freqHz, b.mode);
    if (f == nullptr) { return; }
    if (!std::isnan(f->squelchDb)) { (void)subCommand(FOXAPI_OP_SET_SQUELCH, f->squelchDb, 0); }
    if (!std::isnan(f->toneHz) && f->toneHz > 0.0) {
        const Result r = subCommand(FOXAPI_OP_SET_TONE_SQUELCH, f->toneHz, FOXAPI_TONE_CTCSS, 1);
        if (r.status != FOXAPI_OK) { cascade::core::diagLogf("bookmark: its CTCSS tone could not be applied (status %d)", static_cast<int>(r.status)); }
    }
}

}  // namespace cascade::engine
