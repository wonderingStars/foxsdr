// tools_logic.cpp - the TOOLS bank's pure logic (tools_logic.hpp): the ImGui window's rules and sentences over the table's lists, facts and state, with no window
// and no engine. Every sentence is the drawer's own, through cascade::i18n::tr(); a format is composed with formatText/formatUtf8 into a std::string, never into a
// fixed buffer (tests/test_i18n.cpp refuses a translated format into a sizeof'd one). Each effect function does what the ImGui control does and returns it as
// data (tools_logic.hpp has the table).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tools_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

#include "core/demod_names.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/receiver_position.hpp"
#include "core/tune_control.hpp"
#include "core/utf8_text.hpp"
#include "gui/demod_scope.hpp"
#include "gui/section_reads.hpp"

namespace foxsdr::slinttools {

using cascade::i18n::tr;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;
namespace gui = cascade::gui;

namespace {

constexpr const char* kManualGroup = "Manual";   // app_window_airband.cpp: a name in the user's list, not a label
constexpr std::size_t kChunkBytes = 512u * 1024u;   // the most one ADD_ROWS carries (FOXAPI_MAX_LONG_TEXT is 1 MiB)

std::string T(const char* english) { return tr(english); }

std::string factOf(TableReader& t, const std::string& key) {
    std::string s;
    if (!t.fact(key, s)) { s.clear(); }
    return s;
}

std::int32_t listSize(TableReader& t, std::uint32_t list) { return std::max<std::int32_t>(0, t.readList(list, 0, nullptr)); }

// Every entry of a list, in order; `got <= i` is the end of a list that changed under the read (what was read stands).
template <typename F>
void eachItem(TableReader& t, std::uint32_t list, F&& f) {
    const std::int32_t n = listSize(t, list);
    for (std::int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (t.readList(list, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        f(i, it);
    }
}

Command make(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = foxsdr::slintsignal::grantFor(q.c.op);
    c.valid = true;
    return c;
}
Command make(const FoxCommand& f) {
    cmd::QueuedCommand q;
    q.c = f;
    return make(q);
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// A number as an InputDouble field reads it: blanks off both ends, the whole text a number. 0 ok, 1 empty (not a change), 2 not a number.
int readNumber(const std::string& text, double& out) {
    std::size_t lo = 0, hi = text.size();
    while (lo < hi && std::isspace(static_cast<unsigned char>(text[lo])) != 0) { ++lo; }
    while (hi > lo && std::isspace(static_cast<unsigned char>(text[hi - 1])) != 0) { --hi; }
    if (lo == hi) { return 1; }
    const std::string t = text.substr(lo, hi - lo);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) { return 2; }
    out = v;
    return 0;
}

bool inRange(int index, std::size_t size) { return index >= 0 && static_cast<std::size_t>(index) < size; }

const BookmarkRow* rowOf(const BookmarksModel& m, int id) {
    for (const BookmarkRow& r : m.rows) {
        if (r.id == id) { return &r; }
    }
    return nullptr;
}

// A field of an ADD_ROWS line: the characters that would split the line or the field made spaces (the sender's to clean).
std::string cleanField(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\t' || c == '\r' || c == '\n') { c = ' '; }
    }
    return out;
}

std::string presetOf(const std::string& field) {
    const std::string t = gui::trimPresetName(field);
    return t.empty() ? std::string(kManualGroup) : t;
}

// What the monitor last published (the facts the ImGui section reads on listSeq; read here on every call).
struct AirFacts {
    bool listening = false, pending = false;
    std::string preset, note, block;
    int blocks = 0;
    double squelchDb = -55.0, holdS = 2.0;
};
AirFacts readAirFacts(TableReader& t) {
    AirFacts f;
    f.listening = factOf(t, "fact.airband.listening") == "1";
    f.pending = factOf(t, "fact.airband.pending") == "1";
    f.preset = factOf(t, "fact.airband.preset");
    f.note = factOf(t, "fact.airband.note");
    f.block = factOf(t, "fact.airband.block");
    f.blocks = std::atoi(factOf(t, "fact.airband.blocks").c_str());
    const std::string sq = factOf(t, "fact.airband.squelchDb");
    f.squelchDb = sq.empty() ? -55.0 : std::atof(sq.c_str());
    const std::string hold = factOf(t, "fact.airband.holdS");
    f.holdS = hold.empty() ? 2.0 : std::atof(hold.c_str());
    return f;
}

// One entry of AIRBAND_CHANNELS (read at the call, never cached).
struct Channel {
    double hz = 0.0, bwHz = 0.0, levelDb = -200.0, pendingHeardS = 0.0;
    bool open = false, onAir = false;
    std::string name;
};
std::vector<Channel> readChannels(TableReader& t) {
    std::vector<Channel> out;
    eachItem(t, FOXAPI_LIST_AIRBAND_CHANNELS, [&](int, const FoxListItem& it) {
        Channel c;
        c.hz = it.value[0];
        c.bwHz = it.value[1];
        c.levelDb = it.value[2];
        c.pendingHeardS = it.value[3];
        c.open = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        c.onAir = (it.flags & FOXAPI_ITEM_ON_AIR) != 0u;
        c.name = it.name;
        out.push_back(std::move(c));
    });
    return out;
}

}  // namespace

// =================================================================================================================================
// the small pure rules

std::string coordinateText(double deg) { return cc::formatText("%.5f", deg); }

bool followPosition(const FoxReceiverState& state, PositionSeen& seen, std::string& latText, std::string& lonText) {
    const bool set = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    const bool moved = set && (!seen.set || seen.lat != state.rxLatDeg || seen.lon != state.rxLonDeg);
    if (moved) {
        latText = coordinateText(state.rxLatDeg);
        lonText = coordinateText(state.rxLonDeg);
    }
    seen.set = set;
    seen.lat = set ? state.rxLatDeg : 0.0;
    seen.lon = set ? state.rxLonDeg : 0.0;
    return moved;
}

bool parseCoordinate(const std::string& text, double& out) { return readNumber(text, out) == 0; }

std::string offerPlace(double lat, double lon) {
    return cc::formatText("%.2f%s %.2f%s", std::fabs(lat), cascade::i18n::hemisphereLetter(true, lat >= 0.0), std::fabs(lon),
                          cascade::i18n::hemisphereLetter(false, lon >= 0.0));
}

std::string knownPlace(double lat, double lon) {
    return cc::formatText("%.5f%s %.5f%s", std::fabs(lat), cascade::i18n::hemisphereLetter(true, lat >= 0.0), std::fabs(lon),
                          cascade::i18n::hemisphereLetter(false, lon >= 0.0));
}

std::string positionError(const std::string& latText, const std::string& lonText) {
    double lat = 0.0, lon = 0.0;
    const int a = readNumber(latText, lat);
    const int b = readNumber(lonText, lon);
    if (a == 1 && b == 1) { return std::string(); }   // nothing typed: nothing to complain of
    if (a == 1) { lat = 0.0; }
    if (b == 1) { lon = 0.0; }
    if (a != 2 && b != 2) {
        if (cc::receiverPositionAcceptable(lat, lon)) { return std::string(); }
        if (lat == 0.0 && lon == 0.0) {
            return T("Type the receiver's position first: 0,0 is a point in the\nGulf of Guinea, not a receiver.");
        }
    }
    return T("Latitude must be -90..90 and longitude -180..180");
}

cascade::gui::ModuleCensus trackCensus(const std::vector<foxsdr::slintkit::PluginRow>& rows) {
    cascade::gui::ModuleCensus c;
    for (const foxsdr::slintkit::PluginRow& r : rows) {
        cascade::gui::countModule(c, r.name, r.loaded, r.caps, r.loaded && (r.caps & CASCADE_CAP_TRACK_SOURCE) != 0u, r.stopped, CASCADE_CAP_TRACK_SOURCE);
    }
    return c;
}

std::string scopeChipWord(int signalIndex) { return tr(gui::scopeSignalKey(gui::scopeSignalFromIndex(signalIndex))); }

std::string scannerStateWord(std::uint32_t state) {
    switch (state) {
        case FOXAPI_SCANNER_IDLE: return T("Idle");
        case FOXAPI_SCANNER_SCANNING: return T("Scanning");
        case FOXAPI_SCANNER_PAUSED: return T("Paused (signal)");
        case FOXAPI_SCANNER_HOLDING: return T("Holding");
        default: break;
    }
    return "?";   // a state word a newer engine adds
}

Tone scannerTone(std::uint32_t state) {
    switch (state) {
        case FOXAPI_SCANNER_SCANNING: return Tone::Good;
        case FOXAPI_SCANNER_PAUSED: return Tone::Good;
        case FOXAPI_SCANNER_HOLDING: return Tone::Warn;
        default: break;
    }
    return Tone::Neutral;
}

std::string scannerStatusLine(std::uint32_t state, double currentHz) { return cc::formatText("%s | %.4f MHz", scannerStateWord(state).c_str(), currentHz / 1.0e6); }

std::array<std::string, kScannerFieldCount> scannerFieldTexts(const FoxReceiverState& state) {
    double v[kScannerFieldCount];
    gui::scannerFormValues(state, v);
    return {cc::formatText("%.4f", v[0]), cc::formatText("%.4f", v[1]), cc::formatText("%.2f", v[2]), cc::formatText("%.0f", v[3]),
            cc::formatText("%.0f", v[4]), cc::formatText("%.0f", v[5]), cc::formatText("%.0f", v[6])};
}

std::string trimmedGroupName(const std::string& typed) {
    std::string s = typed;
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && blank(s.back())) { s.pop_back(); }
    std::size_t lead = 0;
    while (lead < s.size() && blank(s[lead])) { ++lead; }
    return s.substr(lead);
}

std::string heardText(double s) {
    if (!(s >= 1.0)) { return "-"; }
    const long long t = static_cast<long long>(s);
    if (t >= 3600) { return cc::formatText("%lldh %02lldm", t / 3600, (t / 60) % 60); }
    if (t >= 60) { return cc::formatText("%lldm %02llds", t / 60, t % 60); }
    return cc::formatText("%llds", t);
}

std::string bookmarkRowLine(const cc::Bookmark& b, bool tick) {
    std::string text = cc::formatText("%.6f\t", b.freqHz);
    text += cleanField(b.mode);
    text += cc::formatText("\t%.3f\t%d\t", b.bandwidthHz, (tick ? 1 : 0) | (b.favourite ? 2 : 0));
    text += cleanField(b.group);
    text += '\t';
    text += cleanField(b.name);
    text += '\n';
    return text;
}

std::vector<std::string> bookmarkRowChunks(const std::vector<cc::Bookmark>& items) {
    std::vector<std::string> chunks;
    std::string cur;
    for (const cc::Bookmark& b : items) {
        const std::string line = bookmarkRowLine(b, b.scan);
        if (!cur.empty() && cur.size() + line.size() > kChunkBytes) {
            chunks.push_back(std::move(cur));
            cur.clear();
        }
        cur += line;
    }
    if (!cur.empty()) { chunks.push_back(std::move(cur)); }
    return chunks;
}

std::string markersClipboard(const MarkersModel& m) {
    std::vector<cc::FreqMarker> list;
    list.reserve(m.rows.size());
    for (const MarkerRow& r : m.rows) {
        cc::FreqMarker f;
        f.number = r.number;
        f.freqHz = r.hz;
        f.notedUnix = r.dropped_unix;
        f.note = r.note;
        list.push_back(std::move(f));
    }
    return cc::formatMarkersClipboard(list);
}

bool airbandPlays(std::uint32_t modeCode) { return modeCode == FOXAPI_DEMOD_AM || modeCode == FOXAPI_DEMOD_NFM; }

// =================================================================================================================================
// the copies

void refreshBookmarks(TableReader& t, BookmarkStore& s) {
    const std::string version = factOf(t, "fact.bookmarks.version");
    if (!s.version.empty() && version == s.version) { return; }
    s.rows.clear();
    s.rows.reserve(static_cast<std::size_t>(listSize(t, FOXAPI_LIST_BOOKMARKS)));
    eachItem(t, FOXAPI_LIST_BOOKMARKS, [&](int, const FoxListItem& it) {
        gui::BookmarkRow m;
        m.hz = it.value[0];
        m.favourite = (it.flags & FOXAPI_ITEM_FAVOURITE) != 0u;
        m.name = it.name;
        m.id = it.id;
        m.group = it.detail;
        m.mode = static_cast<std::uint32_t>(it.value[1]);
        m.bandwidthHz = it.value[2];
        m.ticked = (it.flags & FOXAPI_ITEM_TICKED) != 0u;
        m.heardS = it.value[3];
        s.rows.push_back(std::move(m));
    });
    ++s.copies;
    // A publish between the version and the rows would leave rows newer than the version they are filed under: ask again, and if the store moved,
    // forget the version so the next refresh copies the rows again.
    s.version = (factOf(t, "fact.bookmarks.version") == version) ? version : std::string();
}

void refreshMarkers(TableReader& t, MarkerStore& s) {
    const std::string version = factOf(t, "fact.markers.version");
    if (s.version.empty() || version != s.version) {
        s.rows.clear();
        eachItem(t, FOXAPI_LIST_FREQ_MARKERS, [&](int, const FoxListItem& it) {
            cc::FreqMarker m;
            m.number = static_cast<int>(it.id);
            m.freqHz = it.value[0];
            m.notedUnix = static_cast<std::int64_t>(it.value[1]);
            // The item's name is the note cut at 95 bytes; the whole note is the fact.
            m.note = factOf(t, "fact.markers.note." + std::to_string(it.id));
            s.rows.push_back(std::move(m));
        });
        ++s.copies;
        s.version = (factOf(t, "fact.markers.version") == version) ? version : std::string();
    }
    s.error = factOf(t, "fact.markers.error");
}

// =================================================================================================================================
// the reads

RowChip readScopeChip(const ReadOptions& o) {
    RowChip c;
    c.word = scopeChipWord(o.scope_signal);
    c.tone = Tone::Good;
    c.lit = o.scope_open && o.scope_live;   // the tap actually advancing, with the window open
    return c;
}

PositionOffers readPositionOffers(const FoxReceiverState& state, const ReadOptions& o) {
    PositionOffers p;
    p.known = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    if (p.known) { p.known_text = knownPlace(state.rxLatDeg, state.rxLonDeg); }

    const bool trafficReady = o.traffic_heard >= kTrafficCentreMinAircraft && cc::receiverPositionAcceptable(o.traffic_lat, o.traffic_lon);
    if (trafficReady) {
        p.heard_label = cc::formatText(tr("Use the middle of the %d aircraft heard  (%.2f%s %.2f%s)"), o.traffic_heard, std::fabs(o.traffic_lat),
                                       cascade::i18n::hemisphereLetter(true, o.traffic_lat >= 0.0), std::fabs(o.traffic_lon),
                                       cascade::i18n::hemisphereLetter(false, o.traffic_lon >= 0.0));
    } else if (o.traffic_heard > 0) {
        p.heard_label = cc::formatText(tr("Use the middle of the traffic  (%d heard, need %d)"), o.traffic_heard, kTrafficCentreMinAircraft);
    } else {
        p.heard_label = T("Use the middle of the traffic  (none heard yet)");
    }
    p.heard_enabled = trafficReady;
    p.heard_tooltip = T("The mean position of the aircraft with a reported position.\n"
                        "Reception is roughly a disc about the antenna, so its middle is\n"
                        "near the receiver - a starting point, not a survey.");

    // The map's centre is NOT offered from the origin (a map backed out to the whole world sits on 0,0).
    if (o.map_have && cc::receiverPositionAcceptable(o.map_lat, o.map_lon)) {
        p.map_label = cc::formatText(tr("Use the %s map's centre  (%.2f%s %.2f%s)"), o.map_plugin.c_str(), std::fabs(o.map_lat),
                                     cascade::i18n::hemisphereLetter(true, o.map_lat >= 0.0), std::fabs(o.map_lon),
                                     cascade::i18n::hemisphereLetter(false, o.map_lon >= 0.0));
        p.map_enabled = true;
        p.map_tooltip = T("Where that map is looking right now. If you have been panning\n"
                          "it around your own area, that is close to where you are.");
    }
    p.hint = T("or type it, or click SET FROM MAP CLICK on a map page:");
    p.lat_text = o.lat_text;
    p.lon_text = o.lon_text;
    double lat = 0.0, lon = 0.0;
    const int a = readNumber(o.lat_text, lat);
    const int b = readNumber(o.lon_text, lon);
    if (a == 1) { lat = 0.0; }
    if (b == 1) { lon = 0.0; }
    p.set_enabled = a != 2 && b != 2 && cc::receiverPositionAcceptable(lat, lon);
    p.set_disabled = !p.set_enabled;
    p.error = positionError(o.lat_text, o.lon_text);
    // The key tooltip while it is dead: the ImGui entry holds numbers, so an untouched pair is 0,0 and says so.
    if (!p.set_enabled) { p.set_tooltip = p.error.empty() ? positionError("0", "0") : p.error; }
    p.set_label = T("Set RX here");
    p.gps_shown = true;
    p.gps_hint = T("or read it from a GPS receiver on a serial port:");
    p.gps = o.gps;
    p.footnote = T("All serial-port settings are also under SYSTEM > Serial ports.");
    p.serial_note = p.footnote;
    return p;
}

RadarModel readRadar(const FoxReceiverState& state, const ReadOptions& o) {
    RadarModel r;
    r.offers = readPositionOffers(state, o);
    if (!r.offers.known) { r.warning = T("The scope needs the receiver's position first."); }
    r.scope_on = o.radar_scope_on;
    r.scope_label = o.radar_scope_on ? T("Leave radar scope") : T("Radar scope");
    // SAID HERE RATHER THAN DISCOVERED IN AN EMPTY SCOPE, and it says which kind of nothing (gui/module_census.hpp).
    if (o.track_census.live == 0) {
        r.scope_note = cc::formatText(tr("The scope will be empty. %s"),
                                      gui::trackSourceAbsenceNote(o.track_census, tr("aircraft positions"),
                                                                  tr("An ADS-B decoder is what fills this face; the plugin store is where "
                                                                     "one is fitted from."))
                                          .c_str());
    }
    r.scope_tooltip = T("A plan-position indicator for aircraft: range rings and bearing\n"
                        "ticks around the receiver, with a detail panel for the one you\n"
                        "select. Takes the whole window; there is a way back on its own bar.");
    return r;
}

namespace {

BookmarksModel readBookmarksFrom(TableReader& t, BookmarkStore& store, const ReadOptions& o) {
    refreshBookmarks(t, store);
    const std::vector<gui::BookmarkRow>& list = store.rows;
    BookmarksModel m;
    m.name_draft = o.name_draft;
    m.add_label = T("Add current");
    m.group_draft = o.group_draft;
    const std::vector<std::string> groups = gui::bookmarkGroupNames(list);
    for (const std::string& g : groups) {
        SelectItem s;
        s.label = g;
        s.id = g;
        m.groups.push_back(std::move(s));
    }
    m.import_path = o.import_path;
    m.import_label = T("Import");
    m.import_note = factOf(t, "fact.status.bookmarkNote");
    const bool importOut = o.import_lookup_pending || factOf(t, "fact.bookmarks.importing") == "1";
    m.import_enabled = !importOut;
    m.import_busy = importOut;
    if (importOut && o.import_elapsed_s >= 1.0) { m.disk_wait = cc::formatText(tr("Waiting for the disk to open the file: %.0f s"), o.import_elapsed_s); }
    m.search = o.search;

    // The group the list is filtered to, kept BY NAME (rebuildBookmarkView): the choice survives the list changing under it, and a group just renamed is followed.
    std::string chosen;
    if (!o.filter_group.empty() && contains(groups, o.filter_group)) {
        chosen = o.filter_group;
    } else if (!o.rename_want.empty() && contains(groups, o.rename_want)) {
        chosen = o.rename_want;
    }
    m.filter_group = chosen;
    if (!groups.empty()) {
        SelectItem every;
        every.label = T("Every group");
        every.id = "";
        m.filter_groups.push_back(std::move(every));
        for (const SelectItem& g : m.groups) { m.filter_groups.push_back(g); }
        for (std::size_t i = 0; i < groups.size() && !chosen.empty(); ++i) {
            if (groups[i] == chosen) { m.filter_index = static_cast<int>(i) + 1; }
        }
    }
    m.favourites_only = o.favourites_only;
    m.on_spectrum = o.on_spectrum;
    m.stack_names = o.stack_names;
    m.stack_enabled = o.on_spectrum;   // a control for how names ARE drawn is a lie while none are being drawn

    std::vector<std::uint32_t> view;
    gui::bookmarkView(list, chosen, o.favourites_only, o.search, view);
    m.total = list.size();
    m.shown = view.size();
    m.count_line = cc::formatText(tr("%zu shown of %zu"), view.size(), list.size());
    const bool exporting = factOf(t, "fact.bookmarks.exporting") == "1";
    m.export_enabled = !exporting;
    m.export_busy = exporting;
    if (!view.empty()) { m.export_label = T("Export for SDR#"); }
    if (!chosen.empty()) {
        m.remove_group_label = T("Remove this group");
        m.rename_group_label = T("Rename this group");
    }
    m.rename_open = o.rename_open && !chosen.empty();
    m.rename_draft = o.rename_draft;
    m.rows.reserve(view.size());
    for (const std::uint32_t i : view) {
        if (i >= list.size()) { continue; }
        const gui::BookmarkRow& b = list[i];
        BookmarkRow r;
        r.id = static_cast<int>(b.id);
        r.ticked = b.ticked;
        r.favourite = b.favourite;
        r.name = b.name;
        r.hz = b.hz;
        r.group = b.group;
        r.hz_text = cc::formatText("%.4f MHz", b.hz / 1.0e6);
        r.mode = (b.mode >= 1u && b.mode <= 8u) ? cc::kDemodNames[b.mode] : "";
        r.text = b.group.empty() ? cc::formatText("%s  %.4f MHz", b.name.c_str(), b.hz / 1.0e6)
                                 : cc::formatText("%s  %.4f MHz  [%s]", b.name.c_str(), b.hz / 1.0e6, b.group.c_str());
        m.rows.push_back(std::move(r));
    }
    m.menu_row = o.menu_row;
    m.error = factOf(t, "fact.status.bookmarkError");
    m.chip = std::to_string(list.size());
    m.lamp_lit = !list.empty();
    return m;
}

MarkersModel readMarkersFrom(TableReader& t, MarkerStore& store, const ReadOptions& o) {
    refreshMarkers(t, store);
    MarkersModel m;
    m.shown = o.markers_shown;
    m.error = store.error;
    m.count = store.rows.size();
    // In frequency order: the list is for working through the band.
    std::vector<const cc::FreqMarker*> rows;
    rows.reserve(store.rows.size());
    for (const cc::FreqMarker& f : store.rows) { rows.push_back(&f); }
    std::stable_sort(rows.begin(), rows.end(), [](const cc::FreqMarker* a, const cc::FreqMarker* b) { return a->freqHz < b->freqHz; });
    for (const cc::FreqMarker* f : rows) {
        MarkerRow r;
        r.number = f->number;
        r.label = "M" + std::to_string(f->number);
        r.hz = f->freqHz;
        r.hz_text = cc::markerMhzText(f->freqHz);
        // THE NOTE BEING TYPED is the box's text until the engine's list has it back (markerNoteHold_), for at most thirty frames.
        r.note = (o.note_hold.frames > 0 && o.note_hold.number == f->number) ? o.note_hold.text : f->note;
        r.dropped_unix = f->notedUnix;
        r.dropped = cc::formatMarkerTime(f->notedUnix);
        m.rows.push_back(std::move(r));
    }
    if (m.rows.empty()) {
        m.empty_line = T("No markers yet. Right-click the waterfall and choose Drop marker here.");
    } else {
        m.clear_label = T("Clear all markers");
        m.clear_confirm = cc::formatText(tr("Yes, remove all %zu"), m.rows.size());
        m.clear_yes_label = m.clear_confirm;
        m.copy_label = T("Copy markers to clipboard");
    }
    return m;
}

}  // namespace

BookmarksModel readBookmarks(TableReader& t, const ReadOptions& o) {
    BookmarkStore local;
    return readBookmarksFrom(t, o.cache != nullptr ? o.cache->bookmarks : local, o);
}

MarkersModel readMarkers(TableReader& t, const ReadOptions& o) {
    MarkerStore local;
    return readMarkersFrom(t, o.cache != nullptr ? o.cache->markers : local, o);
}

ScannerModel readScanner(TableReader& t, const FoxReceiverState& state, const ReadOptions& o) {
    ScannerModel m;
    const bool running = (state.flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u;
    m.scanning = running;
    m.ticked_mode = factOf(t, "fact.scanner.listMode") == "1";
    m.ticked_count = static_cast<std::size_t>(std::strtoull(factOf(t, "fact.scanner.ticked").c_str(), nullptr, 10));
    m.ticked_label = cc::formatText(tr("Ticked frequencies (%zu)"), m.ticked_count);
    std::array<std::string, kScannerFieldCount> texts = scannerFieldTexts(state);
    m.draft_bits = o.scan_draft_bits;
    for (int i = 0; i < kScannerFieldCount; ++i) {
        if ((o.scan_draft_bits & (1u << i)) != 0u) { texts[static_cast<std::size_t>(i)] = o.scan_draft[static_cast<std::size_t>(i)]; }
    }
    m.start_mhz = texts[0];
    m.stop_mhz = texts[1];
    m.step_khz = texts[2];
    m.dwell_ms = texts[3];
    m.hold_ms = texts[4];
    m.resume_ms = texts[5];
    m.listen_ms = texts[6];
    m.range_enabled = !m.ticked_mode;
    m.start_label = T("Start scan");
    m.stop_label = T("Stop scan");
    m.skip_label = T("Skip");
    m.start_enabled = !(m.ticked_mode && m.ticked_count == 0);
    m.start_disabled = !m.start_enabled;
    m.status = scannerStatusLine(state.scannerState, state.scanCurrentHz);
    m.status_tone = scannerTone(state.scannerState);
    m.chip = running ? T("SCAN") : T("IDLE");
    m.lamp_lit = running;
    return m;
}

namespace {

AirbandModel readAirbandFrom(TableReader& t, const BookmarkStore& store, const FoxReceiverState& state, const ReadOptions& o) {
    const AirFacts facts = readAirFacts(t);
    const std::vector<Channel> live = readChannels(t);
    const std::vector<gui::BookmarkRow>& list = store.rows;
    AirbandModel m;
    m.listening = facts.listening;
    m.pending = facts.pending;
    m.blocks = facts.blocks;
    m.fact_note = facts.note;
    m.chip = facts.listening ? T("LISTEN") : T("IDLE");
    m.lamp_lit = facts.listening;

    // the airport
    m.code_draft = o.air_code;
    m.lookup_label = T("Look up");
    m.lookup_enabled = !o.air_code.empty();
    m.nearest_label = T("Nearest airports");
    m.nearest_enabled = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    m.nearest_disabled = !m.nearest_enabled;
    if (!o.nearest.empty()) { m.hide_label = T("Hide"); }
    for (const NearestAirport& n : o.nearest) {
        if (n.first == nullptr) { continue; }
        AirportRow r;
        r.code = n.first->ident;
        r.name = n.first->name;
        r.distance = cc::formatText("%.0f km", n.second);
        m.airports.push_back(std::move(r));
    }
    for (const cc::Airport* a : o.choices) {
        if (a == nullptr) { continue; }
        AirportRow r;
        r.code = a->ident;
        r.name = a->name;
        r.note = "(" + a->country + ")";
        m.ambiguous.push_back(std::move(r));
    }

    // the preset, and a frequency of your own for it
    m.preset_name = o.air_preset;
    m.preset = presetOf(o.air_preset);
    m.freq_draft = o.air_freq;
    for (const char* mode : {"AM", "NFM"}) {
        SelectItem s;
        s.label = mode;
        s.id = mode;
        m.modes.push_back(std::move(s));
    }
    m.mode_index = o.air_mode == 1 ? 1 : 0;
    m.name_draft = o.air_name;
    m.add_label = T("Add");
    m.add_enabled = !o.air_freq.empty();
    m.note = o.air_note;
    m.import_path = o.air_import_path;
    m.import_label = T("Import");
    m.import_enabled = !o.air_import_pending;
    m.import_busy = o.air_import_pending;
    m.export_label = T("Export CSV");
    m.remove_preset_label = T("Remove preset");
    m.preset_note = o.air_preset_note;
    if (o.air_folder_key) { m.folder_label = T("Open the folder"); }
    if (o.air_import_pending && o.air_import_elapsed_s >= 1.0) { m.disk_wait = cc::formatText(tr("Waiting for the disk to open the file: %.0f s"), o.air_import_elapsed_s); }

    // which rows: one group, or every ticked AM or NFM row
    std::set<std::string> playable;
    for (const gui::BookmarkRow& b : list) {
        if (airbandPlays(b.mode) && !b.group.empty()) { playable.insert(b.group); }
    }
    // A group that has left the list (removed here, or in the Bookmarks section) is no longer on show.
    const std::string group = (!o.air_group.empty() && playable.count(o.air_group) != 0) ? o.air_group : std::string();
    m.group = group;
    SelectItem every;
    every.label = T("Every ticked AM or NFM frequency");
    every.id = "";
    m.groups.push_back(std::move(every));
    for (const std::string& g : playable) {
        SelectItem s;
        s.label = g;
        s.id = g;
        m.groups.push_back(s);
        m.presets.push_back(std::move(s));
    }
    for (std::size_t i = 0; i < m.groups.size(); ++i) {
        if (m.groups[i].id == group) { m.group_index = static_cast<int>(i); }
    }
    for (std::size_t i = 0; i < m.presets.size(); ++i) {
        if (m.presets[i].id == gui::trimPresetName(o.air_preset)) { m.preset_index = static_cast<int>(i); }
    }

    std::vector<std::uint32_t> rows;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const gui::BookmarkRow& b = list[i];
        if (b.group == m.preset) { ++m.preset_rows; }
        if (!airbandPlays(b.mode)) { continue; }
        if (group.empty() ? b.ticked : (b.group == group)) { rows.push_back(static_cast<std::uint32_t>(i)); }
    }
    // What has been heard but not yet written to the list, so the column moves while the monitor plays rather than every twenty seconds.
    const auto pendingFor = [&](const gui::BookmarkRow& b) {
        for (const Channel& c : live) {
            if (c.hz == b.hz && c.name == b.name) { return c.pendingHeardS; }
        }
        return 0.0;
    };
    const auto chanFor = [&](const gui::BookmarkRow& b) -> const Channel* {
        if (!facts.listening) { return nullptr; }
        for (const Channel& c : live) {
            if (c.hz == b.hz) { return &c; }
        }
        return nullptr;
    };
    m.busiest_first = o.air_busiest;
    if (o.air_busiest) {
        std::stable_sort(rows.begin(), rows.end(),
                         [&](std::uint32_t x, std::uint32_t y) { return list[x].heardS + pendingFor(list[x]) > list[y].heardS + pendingFor(list[y]); });
    }
    std::size_t ticked = 0;
    for (const std::uint32_t i : rows) {
        const gui::BookmarkRow& b = list[i];
        ticked += b.ticked ? 1u : 0u;
        AirbandRow r;
        r.id = static_cast<int>(b.id);
        r.ticked = b.ticked;
        r.hz = b.hz;
        const Channel* c = chanFor(b);
        r.heard = c != nullptr && c->open;
        r.on_air = c != nullptr && c->onAir;
        r.text = cc::formatText("%.4f  %s", b.hz / 1.0e6, b.name.c_str());
        r.heard_time = heardText(b.heardS + pendingFor(b));
        if (c != nullptr) { r.tooltip = cc::formatText(tr("Level %.0f dB (squelch %.0f dB)"), c->levelDb, facts.squelchDb); }
        m.rows.push_back(std::move(r));
    }
    m.count_line = cc::formatText(tr("%zu ticked of %zu"), ticked, rows.size());
    if (!rows.empty() && !group.empty()) {
        m.tick_all_label = T("Tick all");
        m.untick_all_label = T("Untick all");
    }
    m.export_enabled = m.preset_rows != 0 && !o.air_export_waiting && factOf(t, "fact.bookmarks.exporting") != "1";
    m.remove_enabled = m.preset_rows != 0;
    m.export_busy = !m.export_enabled;

    // WHAT IS BEING HEARD, ABOVE THE LIST: the block on the air and the names talking in it; the sentence that waits for the patch while a LISTEN is held.
    if (facts.pending) {
        m.hearing = T("Handing the radio back from the patch view...");
    } else if (facts.listening && !facts.block.empty()) {
        std::string names;
        for (const Channel& c : live) {
            if (!c.open) { continue; }
            if (!names.empty()) { names += ", "; }
            names += c.name;
        }
        m.talking = !names.empty();
        m.block = facts.block;
        m.hearing_line = cc::formatText(tr("Hearing: %s"), m.talking ? names.c_str() : "-");
        m.hearing = m.block + "\n" + m.hearing_line;
    }

    // the monitor's keys
    const double sq = o.squelch_held ? static_cast<double>(o.squelch_edit) : facts.squelchDb;
    m.squelch_from_engine = !o.squelch_held;
    m.squelch_db = sq;
    m.squelch_norm = static_cast<float>(std::clamp((sq - kSquelchMinDb) / (kSquelchMaxDb - kSquelchMinDb), 0.0, 1.0));
    m.squelch_text = cc::formatText(tr("squelch %.0f dB"), sq);
    m.above_noise_label = T("Above the noise");
    m.above_noise_enabled = facts.listening;
    m.hold_s = o.hold_held ? o.hold_edit : facts.holdS;
    m.hold_text = cc::formatText(tr("hold %.1f s"), m.hold_s);
    m.listen_label = (!facts.listening && !facts.pending) ? T("LISTEN") : T("STOP");
    if ((facts.listening || facts.pending) && facts.blocks > 1) { m.next_label = T("Next block"); }
    return m;
}

}  // namespace

AirbandModel readAirband(TableReader& t, const FoxReceiverState& state, const ReadOptions& o) {
    BookmarkStore local;
    BookmarkStore& store = o.cache != nullptr ? o.cache->bookmarks : local;
    refreshBookmarks(t, store);
    return readAirbandFrom(t, store, state, o);
}

ToolsModels readTools(TableReader& t, const FoxReceiverState& state, const ReadOptions& o) {
    ToolsCache local;
    ToolsCache& cache = o.cache != nullptr ? *o.cache : local;
    ToolsModels m;
    m.radar = readRadar(state, o);
    m.bookmarks = readBookmarksFrom(t, cache.bookmarks, o);
    m.scanner = readScanner(t, state, o);
    m.airband = readAirbandFrom(t, cache.bookmarks, state, o);
    m.markers = readMarkersFrom(t, cache.markers, o);
    m.chips[kRowDemodScope] = readScopeChip(o);
    RowChip radar;
    radar.word = T("OFF");   // the one state the rail row can show: the scope replaces the whole window and the rail is never drawn then
    radar.tone = Tone::Neutral;
    radar.lit = false;
    m.chips[kRowRadar] = radar;
    RowChip bm;
    bm.word = m.bookmarks.chip;
    bm.lit = m.bookmarks.lamp_lit;
    m.chips[kRowBookmarks] = bm;
    RowChip sc;
    sc.word = m.scanner.chip;
    sc.lit = m.scanner.lamp_lit;
    m.chips[kRowScanner] = sc;
    RowChip ab;
    ab.word = m.airband.chip;
    ab.lit = m.airband.lamp_lit;
    m.chips[kRowAirband] = ab;
    return m;
}

// =================================================================================================================================
// the effects

namespace {

Effect localEffect(Local l, const std::string& text = std::string(), bool flag = false, int number = 0) {
    Effect e;
    e.local = l;
    e.text = text;
    e.flag = flag;
    e.number = number;
    return e;
}

Effect commandEffect(const Command& c) {
    Effect e;
    e.commands.push_back(c);
    return e;
}

// SET_POSITION, and the coverage that starts again when the pair is one the engine takes (submitReceiverPosition).
Effect positionEffect(double lat, double lon) {
    Effect e;
    e.commands.push_back(make(cmd::makeNum(FOXAPI_OP_SET_POSITION, lat, lon)));
    if (cc::receiverPositionAcceptable(lat, lon)) { e.coverage_reset = true; }
    return e;
}

// An airport's rows as the ADD_ROWS list the lookup sends (airbandAddAirport).
void addAirport(Effect& e, const cc::Airport& a) {
    const std::vector<cc::Bookmark> items = cc::airportBookmarks(a);
    std::string text;
    std::size_t ticked = 0;
    for (const cc::Bookmark& b : items) {
        text += bookmarkRowLine(b, b.scan);
        ticked += b.scan ? 1u : 0u;
    }
    e.clear_choices = true;
    // AS A LIST (ival[0] 1): added as an import adds - a row already there, identical in name, frequency and group, is skipped - and not judged against the radio's range.
    e.commands.push_back(make(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, text, 1)));
    e.ask.kind = AskKind::AirportRows;
    e.ask.group = cc::airportGroupName(a);
    e.ask.total = items.size();
    e.ask.ticked = ticked;
}

}  // namespace

Effect applyScopeClicked() { return localEffect(Local::Deferred, "demod-scope"); }
Effect applyRadarScopeClicked() { return localEffect(Local::Deferred, "radar-scope"); }

Effect applyPositionHeard(const ReadOptions& o) {
    const bool ready = o.traffic_heard >= kTrafficCentreMinAircraft && cc::receiverPositionAcceptable(o.traffic_lat, o.traffic_lon);
    if (!ready) { return {}; }
    return positionEffect(o.traffic_lat, o.traffic_lon);
}

Effect applyPositionMap(const ReadOptions& o) {
    if (!o.map_have || !cc::receiverPositionAcceptable(o.map_lat, o.map_lon)) { return {}; }
    return positionEffect(o.map_lat, o.map_lon);
}

Effect applyPositionLat(const std::string& text) { return localEffect(Local::PositionLat, text); }
Effect applyPositionLon(const std::string& text) { return localEffect(Local::PositionLon, text); }

Effect applyPositionSet(const std::string& latText, const std::string& lonText) {
    double lat = 0.0, lon = 0.0;
    const int a = readNumber(latText, lat);
    const int b = readNumber(lonText, lon);
    if (a == 2 || b == 2) { return {}; }
    if (a == 1) { lat = 0.0; }
    if (b == 1) { lon = 0.0; }
    // REFUSED rather than clamped, and not 0,0: the key simply does not accept the pair.
    if (!cc::receiverPositionAcceptable(lat, lon)) { return {}; }
    return positionEffect(lat, lon);
}

// ---- bookmarks ----

Effect applyBookmarkName(const std::string& text) { return localEffect(Local::BookmarkName, text); }

Effect applyBookmarkAdd(const std::string& nameTyped, const std::string& groupTyped, double tunedHz) {
    // An empty name (or blanks) with the tune known: the bookmark is named by its frequency, the figure its list row shows ("100.3000 MHz") - the engine
    // refuses an empty name silently, which left the classic's and this window's key doing nothing visible (6.46 finding 1).
    std::string name = nameTyped;
    if (name.find_first_not_of(" \t") == std::string::npos && tunedHz > 0.0) { name = cc::formatText("%.4f MHz", tunedHz / 1.0e6); }
    // Under the group box's name when it has one (FOXAPP_OP_BOOKMARK_GROUP, action 0: the engine adds it as BOOKMARK_ADD does, into that group); with none, the ordinary BOOKMARK_ADD.
    const std::string group = trimmedGroupName(groupTyped);
    if (group.empty()) { return commandEffect(make(cmd::makeText(FOXAPI_OP_BOOKMARK_ADD, name))); }
    return commandEffect(make(cmd::makeText(FOXAPP_OP_BOOKMARK_GROUP, group + '\x1F' + name, 0)));
}

Effect applyBookmarkGroup(const std::string& text) { return localEffect(Local::BookmarkGroup, text); }
Effect applyBookmarkImportPath(const std::string& text) { return localEffect(Local::BookmarkImportPath, text); }

Effect applyBookmarkImport(const BookmarksModel& m) {
    if (!m.import_enabled) { return {}; }   // disabled while a lookup is out
    const std::string p = cc::cleanImportPath(m.import_path);
    // Nothing left of what was typed (a pasted line of white space): the box is empty, and an empty box opens the window.
    if (p.empty()) { return localEffect(Local::OpenImportDialog); }
    const std::filesystem::path given{std::u8string(p.begin(), p.end())};
    // A path with a root is opened as it stands: nothing to look for, so nothing for a worker to do.
    if (given.has_root_path()) { return commandEffect(foxsdr::slintsignal::opImportPath(p, 0)); }
    return localEffect(Local::ImportLookup, p);
}

Effect applyBookmarkSearch(const std::string& text) { return localEffect(Local::BookmarkSearch, text); }

Effect applyBookmarkFilter(const BookmarksModel& m, int index) {
    if (index == 0) { return localEffect(Local::BookmarkFilter, ""); }
    if (!inRange(index - 1, m.groups.size())) { return {}; }
    return localEffect(Local::BookmarkFilter, m.groups[static_cast<std::size_t>(index - 1)].id);
}

Effect applyBookmarkFavourites(bool on) { return localEffect(Local::BookmarkFavourites, "", on); }
Effect applyBookmarkOnSpectrum(bool on) { return localEffect(Local::BookmarkOnSpectrum, "", on); }

Effect applyBookmarkStack(const BookmarksModel& m, bool on) {
    if (!m.stack_enabled) { return {}; }   // the box is disabled while "On the spectrum" is off
    return localEffect(Local::BookmarkStack, "", on);
}

Effect applyBookmarkExport(const BookmarksModel& m) {
    if (m.rows.empty() || !m.export_enabled) { return {}; }
    // THE ONES SHOWN go out, so a search or a group picks what is shared; the ENGINE writes the file, from its store.
    std::string ids;
    ids.reserve(m.rows.size() * 7u);
    for (const BookmarkRow& r : m.rows) {
        ids += std::to_string(r.id);
        ids += ' ';
    }
    return commandEffect(make(cmd::makeText(FOXAPP_OP_BOOKMARK_EXPORT, ids)));
}

Effect applyBookmarkRemoveGroup(const BookmarksModel& m) {
    if (m.filter_group.empty()) { return {}; }
    Effect e = localEffect(Local::BookmarkFilter, "");   // the group FILTER going back to "every group" is this panel's own view state
    e.commands.push_back(make(cmd::makeText(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, m.filter_group)));
    return e;
}

Effect applyBookmarkRenameDraft(const std::string& text) { return localEffect(Local::BookmarkRenameDraft, text); }

Effect applyBookmarkRenameOpen(const BookmarksModel& m) {
    if (m.filter_group.empty()) { return {}; }
    // The field opens under the row seeded with the group's name; the key toggles it.
    return localEffect(Local::BookmarkRenameOpen, m.filter_group, !m.rename_open);
}

Effect applyBookmarkRename(const BookmarksModel& m) {
    if (m.filter_group.empty()) { return {}; }
    const std::string from = m.filter_group;
    const std::string to = trimmedGroupName(m.rename_draft);
    if (to.empty() || to == from) { return {}; }
    // FOXAPP_OP_BOOKMARK_GROUP, action 2: the engine renames (or merges), saves and says "Renamed ..." in the note above.
    Effect e = localEffect(Local::BookmarkRenamed, to);
    e.commands.push_back(make(cmd::makeText(FOXAPP_OP_BOOKMARK_GROUP, from + '\x1F' + to, 2)));
    return e;
}

Effect applyBookmarkTick(const BookmarksModel& m, int id, bool on) {
    if (rowOf(m, id) == nullptr) { return {}; }
    return commandEffect(make(cmd::makeInt(FOXAPP_OP_BOOKMARK_TICK, id, on ? 1 : 0)));
}

Effect applyBookmarkFavourite(const BookmarksModel& m, int id, bool on) {
    if (rowOf(m, id) == nullptr) { return {}; }
    return commandEffect(make(cmd::makeInt(FOXAPI_OP_BOOKMARK_FAVOURITE, id, on ? 1 : 0)));
}

Effect applyBookmarkTune(const BookmarksModel& m, int id) {
    if (rowOf(m, id) == nullptr) { return {}; }
    // Click-to-tune: BOOKMARK_TUNE by the entry's id (frequency, then mode, then bandwidth).
    return commandEffect(make(cmd::makeInt(FOXAPI_OP_BOOKMARK_TUNE, id)));
}

Effect applyBookmarkDelete(const BookmarksModel& m, int id) {
    if (rowOf(m, id) == nullptr) { return {}; }
    return commandEffect(make(cmd::makeInt(FOXAPI_OP_BOOKMARK_REMOVE, id)));
}

Effect applyBookmarkMenu(int id) { return localEffect(Local::BookmarkMenu, "", false, id); }

Effect applyBookmarkMove(const BookmarksModel& m, int id, const std::string& groupTyped) {
    Effect e = localEffect(Local::BookmarkMenu, "", false, -1);   // the menu closes either way
    const BookmarkRow* row = rowOf(m, id);
    const std::string group = trimmedGroupName(groupTyped);
    if (row == nullptr || row->group == group) { return e; }
    e.commands.push_back(make(cmd::makeText(FOXAPP_OP_BOOKMARK_GROUP, group, 1, id)));
    return e;
}

// ---- scanner ----

Effect applyScannerTicked(bool on) { return commandEffect(make(cmd::makeInt(FOXAPP_OP_SCANNER_LIST, on ? 1 : 0))); }

Effect applyScannerField(const ScannerModel& m, const FoxReceiverState& state, int field, const std::string& text) {
    if (field < 0 || field >= kScannerFieldCount) { return {}; }
    double value = 0.0;
    if (readNumber(text, value) != 0) { return {}; }   // an empty box, or text that is no number, changes nothing (InputDouble)
    double draft[kScannerFieldCount];
    gui::scannerFormValues(state, draft);   // the draft follows the engine for every field that is not being typed in
    draft[field] = value;
    const unsigned bits = 1u << field;
    Effect e = localEffect(Local::ScannerDraft, text, false, field);
    // THE TIMING HALF FIRST, through FOXAPI_OP_SCANNER_CONFIG (it sets all four and reconfigures a running scan on its own).
    if ((bits & 0x78u) != 0) { e.commands.push_back(make(gui::scannerTimingCommand(bits, draft, state))); }
    // THE RANGE: only the edited field's bit, and - while the scan runs - the reconfigure bit.
    const bool reconfigure = m.scanning;
    if ((bits & 0x07u) != 0) { e.commands.push_back(make(gui::scannerRangeCommand(bits, reconfigure, draft))); }
    return e;
}

Effect applyScannerRun(const ScannerModel& m, const FoxReceiverState& state) {
    if (m.scanning) { return {}; }   // the key is Stop and Skip while the scan runs
    if (m.ticked_mode) {
        if (m.ticked_count == 0) { return {}; }   // disabled with nothing ticked
        // THE TICKED ROWS (the engine also stands the Airband monitor down: one radio, one plan).
        return commandEffect(make(cmd::makeInt(FOXAPP_OP_SCANNER_LIST, 2)));
    }
    // The range the engine stores (the state's), as SCANNER_RUN carries it.
    return commandEffect(make(gui::scannerStartCommand(state)));
}

Effect applyScannerHalt(const ScannerModel& m) {
    if (!m.scanning) { return {}; }
    return commandEffect(make(cmd::makeInt(FOXAPI_OP_SCANNER_RUN, 0)));
}

Effect applyScannerSkip(const ScannerModel& m) {
    if (!m.scanning) { return {}; }
    return commandEffect(make(cmd::make(FOXAPI_OP_SCANNER_SKIP)));
}

// ---- airband ----

Effect applyAirbandCode(const std::string& text) {
    std::string up = text;
    for (char& c : up) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return localEffect(Local::AirbandCode, up);
}

Effect applyAirbandLookup(const std::string& code) {
    if (code.empty()) { return {}; }
    Effect e;
    e.clear_choices = true;
    const std::vector<const cc::Airport*> found = cc::findAirports(cc::airbandTable(), code);
    if (found.empty()) {
        e.set_note = true;
        e.note = cc::formatText(tr("No airport with the code \"%s\" publishes an airband "
                                   "frequency in FoxSDR's table."),
                                code.c_str());
        return e;
    }
    if (found.size() == 1) {
        addAirport(e, *found.front());
        return e;
    }
    e.set_choices = true;
    e.choices = found;
    e.set_note = true;
    e.note = cc::formatText(tr("\"%s\" names %zu airports - pick one:"), code.c_str(), found.size());
    return e;
}

Effect applyAirbandNearest(const FoxReceiverState& state) {
    if ((state.flags & FOXAPI_RX_POSITION_SET) == 0u) { return {}; }   // the key is disabled with no receiver position
    Effect e;
    e.set_nearest = true;
    e.nearest = cc::nearestAirports(cc::airbandTable(), state.rxLatDeg, state.rxLonDeg, kNearestAirports);
    e.clear_choices = true;
    return e;
}

Effect applyAirbandHide() {
    Effect e;
    e.clear_nearest = true;
    return e;
}

Effect applyAirbandAirport(const ReadOptions& o, int index) {
    Effect e;
    if (index >= kChoiceBase) {
        const int k = index - kChoiceBase;
        if (!inRange(k, o.choices.size()) || o.choices[static_cast<std::size_t>(k)] == nullptr) { return {}; }
        addAirport(e, *o.choices[static_cast<std::size_t>(k)]);
        return e;
    }
    if (!inRange(index, o.nearest.size()) || o.nearest[static_cast<std::size_t>(index)].first == nullptr) { return {}; }
    e.clear_nearest = true;
    addAirport(e, *o.nearest[static_cast<std::size_t>(index)].first);
    return e;
}

Effect applyAirbandPreset(const AirbandModel& m, int index) {
    if (!inRange(index, m.presets.size())) { return {}; }
    // The name goes in TRIMMED, as the keys read it; one that does not fit is not copied cut short - the field is cleared.
    return localEffect(Local::AirbandPresetName, gui::presetFieldText(m.presets[static_cast<std::size_t>(index)].id, kPresetFieldBytes));
}

Effect applyAirbandPresetName(const std::string& text) { return localEffect(Local::AirbandPresetName, text); }
Effect applyAirbandFreq(const std::string& text) { return localEffect(Local::AirbandFreq, text); }
Effect applyAirbandName(const std::string& text) { return localEffect(Local::AirbandName, text); }
Effect applyAirbandMode(int index) { return localEffect(Local::AirbandMode, "", false, index == 1 ? 1 : 0); }

Effect applyAirbandAdd(const AirbandModel& m) {
    if (m.freq_draft.empty()) { return {}; }   // disabled with no frequency
    double hz = 0.0;
    if (!gui::parseMhz(m.freq_draft.c_str(), hz)) {
        Effect e;
        e.set_note = true;
        e.note = cc::formatText(tr("could not read frequency \"%s\""), m.freq_draft.c_str());
        return e;
    }
    cc::Bookmark b;
    b.mode = m.mode_index == 1 ? "NFM" : "AM";
    b.freqHz = hz;
    b.bandwidthHz = cc::defaultBandwidthForMode(b.mode);
    b.group = m.preset;
    b.name = m.name_draft;   // empty: the engine names the row by its frequency, as "Add current" does
    Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, bookmarkRowLine(b, true), 0)));
    e.ask.kind = AskKind::TypedRow;
    e.ask.group = b.group;
    e.ask.total = 1;
    e.ask.ticked = 1;
    return e;
}

Effect applyAirbandImportPath(const std::string& text) { return localEffect(Local::AirbandImportPath, text); }

Effect applyAirbandImport(const AirbandModel& m) {
    if (!m.import_enabled) { return {}; }   // disabled while a read is out
    if (m.import_path.empty()) { return localEffect(Local::OpenImportDialog, m.preset, true); }
    Effect e = localEffect(Local::PresetImportLookup, cc::cleanImportPath(m.import_path));
    e.text2 = m.preset;
    e.clear_preset_note = true;
    e.clear_folder_key = true;
    return e;
}

Effect applyAirbandExport(const AirbandModel& m) {
    if (!m.export_enabled) { return {}; }
    Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_BOOKMARK_EXPORT, m.preset, 1)));
    e.ask.kind = AskKind::ExportPreset;
    e.ask.group = m.preset;
    e.clear_folder_key = true;
    return e;
}

Effect applyAirbandRemovePreset(const AirbandModel& m) {
    if (!m.remove_enabled) { return {}; }
    Effect e = commandEffect(make(cmd::makeText(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, m.preset)));
    e.ask.kind = AskKind::RemoveGroup;
    e.ask.group = m.preset;
    e.clear_folder_key = true;
    return e;
}

Effect applyAirbandFolder(TableReader& table) { return localEffect(Local::OpenFolder, factOf(table, "fact.recorder.dir")); }

Effect applyAirbandGroup(const AirbandModel& m, int index) {
    if (!inRange(index, m.groups.size())) { return {}; }
    const std::string g = m.groups[static_cast<std::size_t>(index)].id;
    const bool changed = m.group != g;
    Effect e = localEffect(Local::AirbandGroup, g);
    // THE GROUP SHOWN IS THE PRESET ACTED ON: choosing a group puts its name in the preset field (Add, Export CSV and Remove preset read it there).
    if (index > 0) {
        e.flag = true;
        e.text2 = gui::presetFieldText(g, kPresetFieldBytes);
    }
    // A choice made while the monitor listens is the monitor starting again on it.
    if (changed && (m.listening || m.pending)) {
        e.commands.push_back(make(cmd::makeText(FOXAPP_OP_AIRBAND, g, 1)));
        e.ask.kind = AskKind::Listen;
        e.ask.group = g;
    }
    return e;
}

Effect applyAirbandTickAll(const AirbandModel& m, bool on, bool coverAll) {
    if (m.rows.empty() || m.group.empty()) { return {}; }   // the keys are drawn only with a group on show and rows listed
    Effect e = localEffect(Local::TickRun, "", on);
    for (const AirbandRow& r : m.rows) {
        if (coverAll || r.ticked != on) { e.ids.push_back(static_cast<std::uint64_t>(r.id)); }
    }
    return e;
}

Effect applyAirbandBusiest(bool on) { return localEffect(Local::AirbandBusiest, "", on); }

Effect applyAirbandTick(int id, bool on) { return commandEffect(make(cmd::makeInt(FOXAPP_OP_BOOKMARK_TICK, id, on ? 1 : 0))); }

Effect applyAirbandTune(int id) {
    // One channel, on the receiver: the monitor stops ("Tuned to <name> alone."), because the receiver is now somewhere the monitor did not put it.
    return commandEffect(make(cmd::makeInt(FOXAPP_OP_AIRBAND, 6, id)));
}

Effect applyAirbandSquelch(float norm) {
    if (!std::isfinite(norm)) { return {}; }
    const double db = std::clamp(kSquelchMinDb + static_cast<double>(norm) * (kSquelchMaxDb - kSquelchMinDb), kSquelchMinDb, kSquelchMaxDb);
    // Sent on release, not every frame of a drag: the channels are not restarted sixty times a second.
    Effect e = commandEffect(make(cmd::makeNumInt(FOXAPP_OP_AIRBAND, db, 4)));
    e.local = Local::SquelchHeld;
    e.value = db;
    return e;
}

Effect applyAirbandAboveNoise(const AirbandModel& m) {
    if (!m.listening) { return {}; }
    // The rule is written once, in the engine (8 dB over the median channel level; nothing when no channel has reported yet).
    return commandEffect(make(cmd::makeInt(FOXAPP_OP_AIRBAND, 3)));
}

Effect applyAirbandHold(const std::string& text) {
    // The first number in the text: the box shows "hold 2.0 s", and what is typed over it may keep the words.
    std::size_t at = 0;
    while (at < text.size() && std::strchr("0123456789.+-", text[at]) == nullptr) { ++at; }
    if (at >= text.size()) { return {}; }
    char* end = nullptr;
    const double v = std::strtod(text.c_str() + at, &end);
    if (end == text.c_str() + at || !std::isfinite(v)) { return {}; }
    const double hold = std::clamp(v, kHoldMinS, kHoldMaxS);
    // A hold that is changed while several blocks are being scanned starts the scan again, in the engine.
    Effect e = commandEffect(make(cmd::makeNumInt(FOXAPP_OP_AIRBAND, hold, 5)));
    e.local = Local::HoldHeld;
    e.value = hold;
    return e;
}

Effect applyAirbandListen(const AirbandModel& m) {
    if (!m.listening && !m.pending) {
        Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_AIRBAND, m.group, 1)));
        e.ask.kind = AskKind::Listen;
        e.ask.group = m.group;
        return e;
    }
    return commandEffect(make(cmd::makeInt(FOXAPP_OP_AIRBAND, 0)));
}

Effect applyAirbandNext(const AirbandModel& m) {
    if (!(m.listening || m.pending) || m.blocks <= 1) { return {}; }
    return commandEffect(make(cmd::makeInt(FOXAPP_OP_AIRBAND, 2)));
}

// ---- markers ----

Effect applyMarkersOpen() { return localEffect(Local::MarkersOpen); }

Effect applyMarkerNote(int number, const std::string& text) {
    // A KEYSTROKE AT A TIME, as master saved it: the engine writes half a second after the last change. The box keeps what was typed until the list has it back.
    Effect e = commandEffect(make(cmd::makeText(FOXAPP_OP_MARKER_NOTE, text, number)));
    e.local = Local::MarkerNoteHold;
    e.number = number;
    e.text = text;
    return e;
}

Effect applyMarkerRemove(int number) { return commandEffect(make(cmd::makeInt(FOXAPP_OP_MARKER_REMOVE, number))); }

Effect applyMarkersClear(const MarkersModel& m) {
    if (m.rows.empty()) { return {}; }
    return commandEffect(make(cmd::make(FOXAPP_OP_MARKER_CLEAR)));
}

Effect applyMarkerTune(const MarkersModel& m, int number) {
    for (const MarkerRow& r : m.rows) {
        if (r.number == number) { return commandEffect(make(cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, r.hz))); }
    }
    return {};
}

Effect applyMarkerBookmark(const MarkersModel& m, int number, const FoxReceiverState& state) {
    for (const MarkerRow& r : m.rows) {
        if (r.number != number) { continue; }
        // The note names it when there is one; otherwise its marker number. In the current mode and bandwidth, as master added it. A LIST row (BOOKMARK_ADD_ROWS 1): no range
        // is judged - the marker was dropped where the picture showed - and the same row twice is added once.
        std::string name = !r.note.empty() ? r.note : "M" + std::to_string(r.number);
        for (char& ch : name) {
            if (ch == '\t' || ch == '\n' || ch == '\r') { ch = ' '; }
        }
        const char* mode = state.demodMode >= 1u && state.demodMode <= 8u ? cc::kDemodNames[state.demodMode] : "AM";
        const std::string row = cc::formatText("%.6f", r.hz) + '\t' + mode + '\t' + std::to_string(static_cast<long long>(std::llround(state.bandwidthHz))) + "\t0\t\t" + name;
        return commandEffect(make(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, row, 1)));
    }
    return {};
}

Effect applyMarkersCopy(const MarkersModel& m) {
    if (m.rows.empty()) { return {}; }   // the key is dead with nothing to copy
    return localEffect(Local::Clipboard, markersClipboard(m));
}

Effect applyMarkersClosed() { return localEffect(Local::MarkersClosed); }

}  // namespace foxsdr::slinttools
