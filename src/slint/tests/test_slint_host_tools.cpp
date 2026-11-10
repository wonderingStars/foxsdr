// The TOOLS bank's pure logic (src/slint/tools_logic.hpp) against a STUB TABLE: what the Demod scope row's chip, Radar (the position offers, the scope key), Bookmarks,
// Scanner, Airband and the Markers sheet show, and the exact effect each control has - the op with its arguments and grant, the answer it waits for, the local state the
// publisher keeps - which are the ImGui window's own. No Slint, no engine, no window.
//
// It also READS the ImGui window's sources for every English sentence the logic words (they must be the drawers' own, so a catalogue key can never be one nobody draws),
// and the real airport table for the lookup (the compiled-in FAA / OurAirports data).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tools_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/airband_data.hpp"
#include "core/demod_names.hpp"
#include "core/freq_import.hpp"
#include "core/freq_markers.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/section_reads.hpp"

using namespace foxsdr::slinttools;
namespace core = cascade::core;
using cascade::i18n::tr;
using Names = std::vector<std::string>;

namespace {

// ---- a table of lists and facts a test fills in (the SIGNAL and SYSTEM tests') ----
class StubTable : public TableReader {
public:
    std::map<std::uint32_t, std::vector<FoxListItem>> lists;
    std::map<std::string, std::string> facts;
    std::map<std::uint32_t, int> itemReads;   // how many ENTRIES of each list were read (a count-only call is not one)

    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override {
        const auto it = lists.find(list);
        const std::int32_t n = it == lists.end() ? 0 : static_cast<std::int32_t>(it->second.size());
        if (out != nullptr && static_cast<std::int32_t>(index) < n) {
            *out = it->second[index];
            ++itemReads[list];
        }
        return n;
    }
    bool fact(const std::string& key, std::string& out) override {
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
    void add(std::uint32_t list, std::uint64_t id, const char* name, const char* detail, std::uint32_t flags = 0, double v0 = 0, double v1 = 0, double v2 = 0, double v3 = 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.id = id;
        std::snprintf(it.name, sizeof(it.name), "%s", name);
        std::snprintf(it.detail, sizeof(it.detail), "%s", detail);
        it.flags = flags;
        it.value[0] = v0; it.value[1] = v1; it.value[2] = v2; it.value[3] = v3;
        lists[list].push_back(it);
    }
};

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

FoxReceiverState blankState() {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    return s;
}

std::string commandText(const Command& c) { return foxsdr::slintsignal::textOfCommand(c); }
// A command by index that cannot read past the end: a missing one is an invalid, empty Command and the CHECK that wanted it fails (never a crash).
const Command& cmdOf(const Effect& e, std::size_t i) {
    static const Command none{};
    return i < e.commands.size() ? e.commands[i] : none;
}
const FoxCommand& fcOf(const Effect& e, std::size_t i) { return cmdOf(e, i).q.c; }

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// <repo>/src/<rel>, from this file's own path (.../src/slint/tests/test_slint_host_tools.cpp).
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}

// ---- the fixtures ----
constexpr double kNFM = 1, kWFM = 2, kAM = 3;   // FOXAPI_DEMOD_* (core/demod_names.hpp's order: "", NFM, WFM, AM ...)
constexpr std::uint32_t kFav = FOXAPI_ITEM_FAVOURITE, kTick = FOXAPI_ITEM_TICKED;

// Seven bookmarks: an airport's three rows (one ATIS, unticked), a marine NFM row, two ungrouped (one WFM favourite, one AM ticked), another airport's one.
void fillBookmarks(StubTable& t, const char* version = "5") {
    t.facts["fact.bookmarks.version"] = version;
    t.lists[FOXAPI_LIST_BOOKMARKS].clear();
    t.add(FOXAPI_LIST_BOOKMARKS, 1, "Tower", "KORD", kTick, 118.1e6, kAM, 10000, 100);
    t.add(FOXAPI_LIST_BOOKMARKS, 2, "Ground", "KORD", kTick | kFav, 121.9e6, kAM, 10000, 3700);
    t.add(FOXAPI_LIST_BOOKMARKS, 3, "ATIS", "KORD", 0, 135.4e6, kAM, 10000, 0);
    t.add(FOXAPI_LIST_BOOKMARKS, 4, "Weather ch1", "Marine", kTick, 156.8e6, kNFM, 12500, 45);
    t.add(FOXAPI_LIST_BOOKMARKS, 5, "BBC R2", "", kFav, 88.1e6, kWFM, 150000, 0);
    t.add(FOXAPI_LIST_BOOKMARKS, 6, "Aviation", "", kTick, 121.5e6, kAM, 10000, 0);
    t.add(FOXAPI_LIST_BOOKMARKS, 7, "Lugano", "Lugano", 0, 118.0e6, kAM, 10000, 0);
}

template <typename T>
std::vector<int> idsOf(const std::vector<T>& rows) {
    std::vector<int> out;
    for (const T& r : rows) { out.push_back(r.id); }
    return out;
}
using Ids = std::vector<int>;
using GpsRowCopy = foxsdr::slintsystem::GpsRow;

BookmarksModel bookmarkModel(const ReadOptions& o = ReadOptions{}) {
    StubTable t;
    fillBookmarks(t);
    return readBookmarks(t, o);
}

// ================================================================================================================================
// the sentences are the ImGui window's own
// ================================================================================================================================

// The ImGui sources with every string literal that is split over lines ("a" newline "b") joined, so a sentence can be searched for as one.
std::string joinedSource(const char* rel) {
    std::string s = slurp(srcPath(rel));
    static const std::regex join(R"re("[ \t]*\r?\n[ \t]*")re");
    return std::regex_replace(s, join, "");
}
// A sentence as the source spells it: quotes and breaks escaped.
std::string asSource(const std::string& english) {
    std::string out;
    for (const char c : english) {
        if (c == '"') { out += "\\\""; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}

void sentencesAreTheDrawers() {
    const std::string app = joinedSource("gui/app_window.cpp");
    const std::string air = joinedSource("gui/app_window_airband.cpp");
    const std::string mark = joinedSource("gui/app_window_markers.cpp");
    CHECK(!app.empty() && !air.empty() && !mark.empty());
    const auto in = [&](const std::string& src, const char* english) {
        const std::string lit = "\"" + asSource(english);
        // a label that is a widget id carries its "##suffix" in the source (trId("Hide##airband_near")); the catalogue key is the part before it
        const bool found = src.find(lit + "\"") != std::string::npos || src.find(lit + "##") != std::string::npos;
        if (!found) { std::printf("  sentence not in its drawer: %s\n", english); }
        return found;
    };
    const char* const appKeys[] = {
        "Add current", "Import", "Every group", "Favourites only", "On the spectrum", "Stack close names", "%zu shown of %zu", "Export for SDR#",
        "Remove this group", "Rename this group", "Waiting for the disk to open the file: %.0f s", "Ticked frequencies (%zu)", "Start scan", "Stop scan", "Skip",
        "SCAN", "IDLE", "Idle", "Scanning", "Paused (signal)", "Holding", "Set RX here", "Radar scope", "Leave radar scope", "OFF",
        "The scope needs the receiver's position first.", "The scope will be empty. %s", "aircraft positions",
        "An ADS-B decoder is what fills this face; the plugin store is where one is fitted from.",
        "Use the middle of the %d aircraft heard  (%.2f%s %.2f%s)", "Use the middle of the traffic  (%d heard, need %d)",
        "Use the middle of the traffic  (none heard yet)", "Use the %s map's centre  (%.2f%s %.2f%s)",
        "or type it, or click SET FROM MAP CLICK on a map page:", "All serial-port settings are also under SYSTEM > Serial ports.",
        "Latitude must be -90..90 and longitude -180..180",
        "Type the receiver's position first: 0,0 is a point in the\nGulf of Guinea, not a receiver.",
        "The mean position of the aircraft with a reported position.\nReception is roughly a disc about the antenna, so its middle is\nnear the receiver - a starting point, not a survey.",
        "Where that map is looking right now. If you have been panning\nit around your own area, that is close to where you are.",
        "A plan-position indicator for aircraft: range rings and bearing\nticks around the receiver, with a detail panel for the one you\nselect. Takes the whole window; there is a way back on its own bar.",
    };
    for (const char* k : appKeys) { CHECK(in(app, k)); }
    const char* const airKeys[] = {
        "Look up", "Nearest airports", "Hide", "Add", "Open the folder", "Export CSV", "Remove preset", "Every ticked AM or NFM frequency", "%zu ticked of %zu", "Tick all", "Untick all",
        "Hearing: %s", "Handing the radio back from the patch view...", "Level %.0f dB (squelch %.0f dB)", "squelch %.0f dB", "Above the noise", "hold %.1f s", "LISTEN", "STOP",
        "Next block", "could not read frequency \"%s\"", "\"%s\" names %zu airports - pick one:",
        "No airport with the code \"%s\" publishes an airband frequency in FoxSDR's table.", "Waiting for the disk to open the file: %.0f s",
    };
    for (const char* k : airKeys) { CHECK(in(air, k)); }
    const char* const markKeys[] = {"Clear all markers", "Yes, remove all %zu", "No markers yet. Right-click the waterfall and choose Drop marker here."};
    for (const char* k : markKeys) { CHECK(in(mark, k)); }

    // The ImGui literals the commands carry: the group separator, the scanner's masks, the airband actions - read from the sources they are in.
    CHECK(has(app, "group + '\\x1F' + bookmarkName_"));
    CHECK(has(app, "from + '\\x1F' + to, 2"));
    CHECK(has(app, "FOXAPP_OP_SCANNER_LIST, 2"));
    CHECK(has(air, "FOXAPP_OP_AIRBAND, 6, tuneId"));
    CHECK(has(air, "a.squelchEdit), 4"));
    CHECK(has(air, "a.holdEdit, 5"));
    CHECK(has(air, "FOXAPP_OP_AIRBAND, 3"));
    CHECK(has(air, "FOXAPP_OP_AIRBAND, 2"));
    CHECK(has(air, "FOXAPP_OP_AIRBAND, 0"));
    CHECK(has(air, "kManualGroup = \"Manual\""));
    CHECK(has(air, "airportBookmarks(a)"));
    CHECK(has(air, "nearestAirports(cc::airbandTable(), uiState_.rxLatDeg, uiState_.rxLonDeg, 6)"));
    CHECK(has(mark, "FOXAPP_OP_MARKER_NOTE, note, m->number"));
    CHECK(has(mark, "FOXAPI_OP_SET_FREQUENCY, m->freqHz"));
    CHECK(has(mark, "markerNoteHold_.frames = 30"));
    CHECK(kNoteHoldFrames == 30 && kNearestAirports == 6 && kPresetFieldBytes == 128);
    CHECK(kTrafficCentreMinAircraft == 3 && has(joinedSource("gui/scope_view.hpp"), "kScopeTrafficCentreMinAircraft = 3"));
}

// EVERY tr("...") / T("...") LITERAL OF tools_logic.cpp is a sentence one of the three drawers says: the logic adds no key. (A literal split over lines is joined first, as the
// drawers' own are; a catalogue key is what stands before a widget label's "##suffix".)
void everySentenceIsADrawers() {
    const std::string mine = joinedSource("slint/tools_logic.cpp");
    CHECK(!mine.empty());
    const std::string drawers = joinedSource("gui/app_window.cpp") + joinedSource("gui/app_window_airband.cpp") + joinedSource("gui/app_window_markers.cpp");
    static const std::regex literal(R"re(\b(?:tr|T)\(\s*("(?:[^"\\]|\\.)*")\s*\))re");
    int seen = 0, missing = 0;
    for (std::sregex_iterator it(mine.begin(), mine.end(), literal), end; it != end; ++it) {
        const std::string quoted = (*it)[1];   // the literal as the source spells it, quotes included
        const std::string body = quoted.substr(0, quoted.size() - 1);
        ++seen;
        if (drawers.find(quoted) == std::string::npos && drawers.find(body + "##") == std::string::npos) {
            std::printf("  a sentence of tools_logic.cpp that no drawer says: %s\n", quoted.c_str());
            ++missing;
        }
    }
    CHECK(seen >= 60);   // the whole of them were found (the pattern did not quietly match nothing)
    CHECK(missing == 0);
}

// Every sentence of the logic has a translation: the German catalogue carries it (a key nobody translated reads as English in a German window).
void sentencesAreTranslated() {
    const std::string was = cascade::i18n::current();
    cascade::i18n::setLanguage("de");
    int untranslated = 0;
    const char* const keys[] = {"Add current", "Import", "Every group", "Export for SDR#", "Remove this group", "Rename this group", "Start scan", "Stop scan", "Skip",
                                "Look up", "Nearest airports", "Export CSV", "Remove preset", "Above the noise", "Next block", "Clear all markers", "Set RX here",
                                "Radar scope", "Leave radar scope", "The scope needs the receiver's position first.", "Idle", "Scanning", "Holding", "Paused (signal)",
                                "%zu shown of %zu", "%zu ticked of %zu", "Ticked frequencies (%zu)", "Tick all", "Untick all"};
    for (const char* k : keys) {
        if (std::string(tr(k)) == k) {
            std::printf("  not translated into German: %s\n", k);
            ++untranslated;
        }
    }
    CHECK(untranslated == 0);
    // ...and the model says it in the language in force.
    StubTable t;
    fillBookmarks(t);
    const BookmarksModel m = readBookmarks(t, ReadOptions{});
    CHECK(m.add_label == tr("Add current") && m.add_label != "Add current");
    cascade::i18n::setLanguage(was);
    CHECK(readBookmarks(t, ReadOptions{}).add_label == "Add current");
}

// ================================================================================================================================
// the reads
// ================================================================================================================================

void scopeAndRadarReads() {
    ReadOptions o;
    const char* const words[] = {"AUDIO", "SPEC", "I/Q", "VECTOR", "MPX"};
    for (int i = 0; i < 5; ++i) {
        o.scope_signal = i;
        CHECK(readScopeChip(o).word == words[i]);
        CHECK(scopeChipWord(i) == words[i]);
    }
    o.scope_signal = 99;   // a hand-edited config clamps (core::scopeSignalFromIndex): past the end is the last face but one, before the start the first
    CHECK(readScopeChip(o).word == "VECTOR");
    o.scope_signal = -4;
    CHECK(readScopeChip(o).word == "AUDIO");
    // The lamp: the window is open AND the tap is advancing.
    o.scope_signal = 0;
    CHECK(!readScopeChip(o).lit);
    o.scope_open = true;
    CHECK(!readScopeChip(o).lit);
    o.scope_live = true;
    CHECK(readScopeChip(o).lit);
    o.scope_open = false;
    CHECK(!readScopeChip(o).lit);

    // No position: the warning, the offers' labels in each of their three states, the typed entry.
    FoxReceiverState s = blankState();
    ReadOptions r;
    RadarModel m = readRadar(s, r);
    CHECK(m.warning == "The scope needs the receiver's position first." && !m.offers.known && m.offers.known_text.empty());
    CHECK(m.scope_label == "Radar scope" && !m.scope_on);
    CHECK(m.offers.heard_label == "Use the middle of the traffic  (none heard yet)" && !m.offers.heard_enabled);
    CHECK(m.offers.map_label.empty() && !m.offers.map_enabled);
    CHECK(m.offers.hint == "or type it, or click SET FROM MAP CLICK on a map page:");
    CHECK(m.offers.set_label == "Set RX here" && m.offers.serial_note == "All serial-port settings are also under SYSTEM > Serial ports.");
    // the fields the UI family added: the footnote, the GPS hint, and the key that says why it is dead (an untouched pair is 0,0 in the ImGui entry)
    CHECK(m.offers.footnote == m.offers.serial_note && m.offers.gps_shown && m.offers.gps_hint == "or read it from a GPS receiver on a serial port:");
    CHECK(m.offers.set_disabled && !m.offers.set_enabled && m.offers.set_tooltip == "Type the receiver's position first: 0,0 is a point in the\nGulf of Guinea, not a receiver.");
    r.traffic_heard = 2;
    r.traffic_lat = 51.5;
    r.traffic_lon = -0.12;
    m = readRadar(s, r);
    CHECK(m.offers.heard_label == "Use the middle of the traffic  (2 heard, need 3)" && !m.offers.heard_enabled);
    r.traffic_heard = 4;
    m = readRadar(s, r);
    CHECK(m.offers.heard_label == "Use the middle of the 4 aircraft heard  (51.50N 0.12W)" && m.offers.heard_enabled);
    r.traffic_lat = -33.9;
    r.traffic_lon = 151.2;
    CHECK(readRadar(s, r).offers.heard_label == "Use the middle of the 4 aircraft heard  (33.90S 151.20E)");
    // Enough aircraft but a middle that is the origin: not offered (receiverPositionAcceptable), and said as the ImGui button says it.
    r.traffic_heard = 5;
    r.traffic_lat = 0.0;
    r.traffic_lon = 0.0;
    m = readRadar(s, r);
    CHECK(!m.offers.heard_enabled && m.offers.heard_label == "Use the middle of the traffic  (5 heard, need 3)");

    // The map's centre: the page's name and place; not from the origin; not with no page.
    r.map_have = true;
    r.map_plugin = "ADS-B";
    r.map_lat = 53.1;
    r.map_lon = 8.2;
    m = readRadar(s, r);
    CHECK(m.offers.map_label == "Use the ADS-B map's centre  (53.10N 8.20E)" && m.offers.map_enabled);
    r.map_lat = 0.0;
    r.map_lon = 0.0;
    CHECK(readRadar(s, r).offers.map_label.empty() && !readRadar(s, r).offers.map_enabled);
    r.map_lat = 53.1;
    r.map_have = false;
    CHECK(readRadar(s, r).offers.map_label.empty());

    // The typed entry, and why the key is dead.
    ReadOptions e;
    CHECK(readPositionOffers(s, e).error.empty() && !readPositionOffers(s, e).set_enabled);   // nothing typed
    e.lat_text = "0";
    e.lon_text = "0";
    CHECK(readPositionOffers(s, e).error == "Type the receiver's position first: 0,0 is a point in the\nGulf of Guinea, not a receiver.");
    CHECK(!readPositionOffers(s, e).set_enabled);
    e.lat_text = "95";
    e.lon_text = "10";
    CHECK(readPositionOffers(s, e).error == "Latitude must be -90..90 and longitude -180..180" && !readPositionOffers(s, e).set_enabled);
    e.lat_text = "abc";
    CHECK(readPositionOffers(s, e).error == "Latitude must be -90..90 and longitude -180..180");
    e.lat_text = "51.5";
    e.lon_text = "-181";
    CHECK(readPositionOffers(s, e).error == "Latitude must be -90..90 and longitude -180..180");
    e.lon_text = "-0.1";
    PositionOffers p = readPositionOffers(s, e);
    CHECK(p.error.empty() && p.set_enabled && p.lat_text == "51.5" && p.lon_text == "-0.1");
    e.lon_text = "";   // an emptied box is 0, as InputDouble leaves it
    CHECK(readPositionOffers(s, e).set_enabled);

    // the GPS row is the SYSTEM bank's, passed through as it was read
    ReadOptions gp;
    gp.gps.listening = true;
    gp.gps.status = "Listening on COM3";
    gp.gps.satellites = "7";
    gp.gps.port_index = 2;
    const GpsRowCopy gpsOut = readPositionOffers(s, gp).gps;
    CHECK(gpsOut.listening && gpsOut.status == "Listening on COM3" && gpsOut.satellites == "7" && gpsOut.port_index == 2);
    // the key is live, so no tooltip; dead for a range error, with that error
    ReadOptions live;
    live.lat_text = "51.5";
    live.lon_text = "-0.1";
    CHECK(!readPositionOffers(s, live).set_disabled && readPositionOffers(s, live).set_tooltip.empty());
    live.lat_text = "95";
    CHECK(readPositionOffers(s, live).set_disabled && readPositionOffers(s, live).set_tooltip == "Latitude must be -90..90 and longitude -180..180");

    // A position known: no warning, the place in words, the fold.
    s.flags |= FOXAPI_RX_POSITION_SET;
    s.rxLatDeg = 51.5;
    s.rxLonDeg = -0.12;
    m = readRadar(s, r);
    CHECK(m.warning.empty() && m.offers.known && m.offers.known_text == "51.50000N 0.12000W");
    s.rxLatDeg = -33.865;
    s.rxLonDeg = 151.209;
    CHECK(readRadar(s, r).offers.known_text == "33.86500S 151.20900E");

    // The scope key and the note under it: the census says which kind of nothing.
    ReadOptions c;
    m = readRadar(s, c);
    CHECK(m.scope_note.rfind("The scope will be empty. ", 0) == 0 && m.scope_note.size() > 30);
    c.track_census.live = 1;
    CHECK(readRadar(s, c).scope_note.empty());
    c.track_census.live = 0;
    c.track_census.stopped = 1;
    c.track_census.stoppedName = "ADS-B";
    CHECK(has(readRadar(s, c).scope_note, "ADS-B"));
    c.radar_scope_on = true;
    CHECK(readRadar(s, c).scope_label == "Leave radar scope" && readRadar(s, c).scope_on);

    // trackCensus: the rule is the census of the PLUGINS rows with the track-source capability.
    std::vector<foxsdr::slintkit::PluginRow> rows(3);
    rows[0].name = "ADS-B";
    rows[0].loaded = true;
    rows[0].caps = CASCADE_CAP_TRACK_SOURCE;
    rows[1].name = "Ships";
    rows[1].loaded = true;
    rows[1].stopped = true;
    rows[1].caps = CASCADE_CAP_TRACK_SOURCE;
    rows[2].name = "RTTY";
    rows[2].loaded = true;
    rows[2].caps = CASCADE_CAP_DECODER;
    const cascade::gui::ModuleCensus census = trackCensus(rows);
    CHECK(census.live == 1 && census.stopped == 1 && census.stoppedName == "Ships");
    rows[0].loaded = false;
    CHECK(trackCensus(rows).live == 0);

    // followPosition: the typed fields follow the state when it moves, once.
    PositionSeen seen;
    std::string lat, lon;
    FoxReceiverState f = blankState();
    CHECK(!followPosition(f, seen, lat, lon) && lat.empty());
    f.flags |= FOXAPI_RX_POSITION_SET;
    f.rxLatDeg = 12.34567891;
    f.rxLonDeg = -98.7;
    CHECK(followPosition(f, seen, lat, lon) && lat == "12.34568" && lon == "-98.70000");
    lat = "typed";
    CHECK(!followPosition(f, seen, lat, lon) && lat == "typed");
    f.rxLonDeg = 5.0;
    CHECK(followPosition(f, seen, lat, lon) && lon == "5.00000");
    f.flags = 0;
    CHECK(!followPosition(f, seen, lat, lon) && !seen.set);
}

void bookmarkReads() {
    StubTable t;
    fillBookmarks(t);
    t.facts["fact.status.bookmarkNote"] = "Imported 3 entries";
    t.facts["fact.status.bookmarkError"] = "could not save";
    ReadOptions o;
    o.name_draft = "my name";
    o.group_draft = "KORD";
    o.import_path = "x.csv";
    o.rename_draft = "Chicago";
    BookmarksModel m = readBookmarks(t, o);
    CHECK(m.count_line == "7 shown of 7" && m.chip == "7" && m.lamp_lit && m.total == 7 && m.shown == 7);
    CHECK(m.add_label == "Add current" && m.import_label == "Import" && m.name_draft == "my name" && m.group_draft == "KORD" && m.import_path == "x.csv" && m.rename_draft == "Chicago");
    CHECK(m.import_note == "Imported 3 entries" && m.error == "could not save");
    Names groups;
    for (const SelectItem& g : m.groups) { groups.push_back(g.id); }
    CHECK(groups == (Names{"KORD", "Lugano", "Marine"}));
    Names filter;
    for (const SelectItem& g : m.filter_groups) { filter.push_back(g.label); }
    CHECK(filter == (Names{"Every group", "KORD", "Lugano", "Marine"}) && m.filter_index == 0 && m.filter_group.empty());
    CHECK(m.rows.size() == 7);
    CHECK(m.rows.size() == 7 && m.rows[0].text == "Tower  118.1000 MHz  [KORD]" && m.rows[0].id == 1 && m.rows[0].ticked && !m.rows[0].favourite);
    CHECK(m.rows.size() == 7 && m.rows[4].text == "BBC R2  88.1000 MHz" && m.rows[4].favourite && !m.rows[4].ticked && m.rows[4].group.empty());
    CHECK(m.rows.size() == 7 && m.rows[0].hz_text == "118.1000 MHz" && m.rows[0].mode == "AM" && m.rows[3].mode == "NFM" && m.rows[4].mode == "WFM");
    CHECK(m.rows.size() == 7 && m.rows[1].favourite && m.rows[1].ticked && m.rows[1].name == "Ground" && m.rows[1].hz == 121.9e6);
    CHECK(m.export_label == "Export for SDR#" && m.export_enabled && m.import_enabled);
    CHECK(m.remove_group_label.empty() && m.rename_group_label.empty() && !m.rename_open);
    CHECK(m.favourites_only == false && m.on_spectrum && m.stack_names && m.stack_enabled && m.menu_row == -1);

    // a search: names, groups, case-blind, and a frequency by its MHz prefix
    ReadOptions s;
    s.search = "TOWER";
    m = readBookmarks(t, s);
    CHECK(idsOf(m.rows) == (Ids{1}) && m.count_line == "1 shown of 7" && m.search == "TOWER");
    s.search = "kord";
    CHECK(idsOf(readBookmarks(t, s).rows) == (Ids{1, 2, 3}));
    s.search = "121";
    CHECK(idsOf(readBookmarks(t, s).rows) == (Ids{2, 6}));   // 121.9 and 121.5 MHz
    s.search = "nothing matches this";
    m = readBookmarks(t, s);
    CHECK(m.rows.empty() && m.count_line == "0 shown of 7" && m.export_label.empty());

    // a group, favourites only, both - the choice kept BY NAME
    ReadOptions g;
    g.filter_group = "KORD";
    m = readBookmarks(t, g);
    CHECK(idsOf(m.rows) == (Ids{1, 2, 3}) && m.count_line == "3 shown of 7" && m.filter_index == 1 && m.filter_group == "KORD");
    CHECK(m.remove_group_label == "Remove this group" && m.rename_group_label == "Rename this group");
    g.filter_group = "Marine";
    CHECK(readBookmarks(t, g).filter_index == 3 && idsOf(readBookmarks(t, g).rows) == (Ids{4}));
    g.favourites_only = true;
    g.filter_group = "KORD";
    CHECK(idsOf(readBookmarks(t, g).rows) == (Ids{2}));
    g.filter_group.clear();
    CHECK(idsOf(readBookmarks(t, g).rows) == (Ids{2, 5}) && readBookmarks(t, g).favourites_only);
    // a group the list no longer holds is "every group"; a group just renamed is followed once the list has it
    ReadOptions gone;
    gone.filter_group = "Vanished";
    m = readBookmarks(t, gone);
    CHECK(m.filter_group.empty() && m.filter_index == 0 && m.rows.size() == 7);
    gone.rename_want = "Lugano";
    m = readBookmarks(t, gone);
    CHECK(m.filter_group == "Lugano" && m.filter_index == 2 && idsOf(m.rows) == (Ids{7}));
    gone.rename_want = "Not yet";
    CHECK(readBookmarks(t, gone).filter_group.empty());
    // the rename field is drawn only while a group is chosen
    ReadOptions rn;
    rn.rename_open = true;
    CHECK(!readBookmarks(t, rn).rename_open);
    rn.filter_group = "KORD";
    CHECK(readBookmarks(t, rn).rename_open);

    // the empty list: nothing to export, no group combo, no lamp
    StubTable none;
    none.facts["fact.bookmarks.version"] = "1";
    m = readBookmarks(none, ReadOptions{});
    CHECK(m.chip == "0" && !m.lamp_lit && m.count_line == "0 shown of 0" && m.export_label.empty() && m.filter_groups.empty() && m.groups.empty() && m.rows.empty());

    // the import and export in flight
    ReadOptions imp;
    imp.import_elapsed_s = 0.4;
    t.facts["fact.bookmarks.importing"] = "1";
    m = readBookmarks(t, imp);
    CHECK(!m.import_enabled && m.import_busy && m.disk_wait.empty());
    imp.import_elapsed_s = 2.4;
    m = readBookmarks(t, imp);
    CHECK(!m.import_enabled && m.disk_wait == "Waiting for the disk to open the file: 2 s");
    t.facts["fact.bookmarks.importing"] = "0";
    CHECK(readBookmarks(t, imp).import_enabled && readBookmarks(t, imp).disk_wait.empty());
    imp.import_lookup_pending = true;
    m = readBookmarks(t, imp);
    CHECK(!m.import_enabled && m.disk_wait == "Waiting for the disk to open the file: 2 s");
    t.facts["fact.bookmarks.exporting"] = "1";
    CHECK(!readBookmarks(t, ReadOptions{}).export_enabled && readBookmarks(t, ReadOptions{}).export_busy);
    t.facts["fact.bookmarks.exporting"] = "0";
    CHECK(readBookmarks(t, ReadOptions{}).export_enabled && !readBookmarks(t, ReadOptions{}).export_busy && !readBookmarks(t, ReadOptions{}).import_busy);

    // "On the spectrum" off: the stack box stands aside
    ReadOptions sp;
    sp.on_spectrum = false;
    sp.stack_names = false;
    sp.menu_row = 4;
    m = readBookmarks(t, sp);
    CHECK(!m.on_spectrum && !m.stack_enabled && !m.stack_names && m.menu_row == 4);

    // a name cut at the item size is the item's; the copy is read again only when the store's own counter moves
    StubTable c;
    fillBookmarks(c, "9");
    ToolsCache cache;
    ReadOptions co;
    co.cache = &cache;
    CHECK(readBookmarks(c, co).rows.size() == 7 && c.itemReads[FOXAPI_LIST_BOOKMARKS] == 7 && cache.bookmarks.copies == 1);
    CHECK(readBookmarks(c, co).rows.size() == 7 && c.itemReads[FOXAPI_LIST_BOOKMARKS] == 7 && cache.bookmarks.copies == 1);   // same version: no entry read again
    CHECK(readBookmarks(c, co).count_line == "7 shown of 7");
    c.add(FOXAPI_LIST_BOOKMARKS, 8, "New", "", 0, 100e6, kAM, 10000);
    CHECK(readBookmarks(c, co).rows.size() == 7 && cache.bookmarks.copies == 1);   // the list moved but the version did not: the copy stands
    c.facts["fact.bookmarks.version"] = "10";
    CHECK(readBookmarks(c, co).rows.size() == 8 && cache.bookmarks.copies == 2);
    // with no cache, every read copies
    const int before = c.itemReads[FOXAPI_LIST_BOOKMARKS];
    readBookmarks(c, ReadOptions{});
    CHECK(c.itemReads[FOXAPI_LIST_BOOKMARKS] == before + 8);
    // a store with no version fact is never trusted
    StubTable nv;
    nv.add(FOXAPI_LIST_BOOKMARKS, 1, "A", "", 0, 100e6, kAM, 10000);
    ToolsCache nvc;
    ReadOptions nvo;
    nvo.cache = &nvc;
    readBookmarks(nv, nvo);
    readBookmarks(nv, nvo);
    CHECK(nvc.bookmarks.copies == 2);
}

void scannerReads() {
    StubTable t;
    FoxReceiverState s = blankState();
    s.scanStartHz = 88.0e6;
    s.scanStopHz = 108.0e6;
    s.scanStepHz = 100.0e3;
    s.scanDwellMs = 150;
    s.scanHoldMs = 2000;
    s.scanResumeMs = 500;
    s.scanListenMs = 0;
    s.scanCurrentHz = 98.4e6;
    s.scannerState = FOXAPI_SCANNER_IDLE;
    ScannerModel m = readScanner(t, s, ReadOptions{});
    CHECK(m.start_mhz == "88.0000" && m.stop_mhz == "108.0000" && m.step_khz == "100.00" && m.dwell_ms == "150" && m.hold_ms == "2000" && m.resume_ms == "500" && m.listen_ms == "0");
    CHECK(!m.scanning && m.status == "Idle | 98.4000 MHz" && m.status_tone == Tone::Neutral && m.chip == "IDLE" && !m.lamp_lit);
    CHECK(m.start_label == "Start scan" && m.stop_label == "Stop scan" && m.skip_label == "Skip");
    CHECK(!m.ticked_mode && m.ticked_label == "Ticked frequencies (0)" && m.range_enabled && m.start_enabled && m.ticked_count == 0);

    s.flags |= FOXAPI_RX_SCANNER_ACTIVE;
    s.scannerState = FOXAPI_SCANNER_SCANNING;
    m = readScanner(t, s, ReadOptions{});
    CHECK(m.scanning && m.status == "Scanning | 98.4000 MHz" && m.status_tone == Tone::Good && m.chip == "SCAN" && m.lamp_lit);
    s.scannerState = FOXAPI_SCANNER_PAUSED;
    m = readScanner(t, s, ReadOptions{});
    CHECK(m.status == "Paused (signal) | 98.4000 MHz" && m.status_tone == Tone::Good);
    s.scannerState = FOXAPI_SCANNER_HOLDING;
    m = readScanner(t, s, ReadOptions{});
    CHECK(m.status == "Holding | 98.4000 MHz" && m.status_tone == Tone::Warn);
    s.scannerState = 77;   // a state word a newer engine adds
    CHECK(readScanner(t, s, ReadOptions{}).status == "? | 98.4000 MHz" && readScanner(t, s, ReadOptions{}).status_tone == Tone::Neutral);
    CHECK(scannerStateWord(FOXAPI_SCANNER_IDLE) == "Idle" && scannerStateWord(9) == "?");

    // list mode: the range stands aside; nothing ticked: Start is dead
    s.flags = 0;
    s.scannerState = FOXAPI_SCANNER_IDLE;
    t.facts["fact.scanner.listMode"] = "1";
    t.facts["fact.scanner.ticked"] = "0";
    m = readScanner(t, s, ReadOptions{});
    CHECK(m.ticked_mode && !m.range_enabled && !m.start_enabled && m.start_disabled && m.ticked_label == "Ticked frequencies (0)");
    t.facts["fact.scanner.ticked"] = "3";
    m = readScanner(t, s, ReadOptions{});
    CHECK(m.start_enabled && !m.start_disabled && m.ticked_count == 3 && m.ticked_label == "Ticked frequencies (3)");

    // a typed draft stands in for the engine's value until the engine's figure has caught up
    ReadOptions d;
    d.scan_draft_bits = (1u << 0) | (1u << 6);
    d.scan_draft[0] = "144.";
    d.scan_draft[6] = "2500";
    m = readScanner(t, s, d);
    CHECK(m.start_mhz == "144." && m.listen_ms == "2500" && m.stop_mhz == "108.0000" && m.draft_bits == d.scan_draft_bits);
}

// A bookmark list and an airband monitor mid-listen.
void fillAirband(StubTable& t, bool listening) {
    fillBookmarks(t);
    t.facts["fact.airband.listening"] = listening ? "1" : "0";
    t.facts["fact.airband.pending"] = "0";
    t.facts["fact.airband.preset"] = "";
    t.facts["fact.airband.note"] = "";
    t.facts["fact.airband.block"] = listening ? "Block 1 of 2: 3 channels, 118.000-121.900 MHz" : "";
    t.facts["fact.airband.blocks"] = listening ? "2" : "0";
    t.facts["fact.airband.squelchDb"] = "-55.0";
    t.facts["fact.airband.holdS"] = "2.0";
    t.lists[FOXAPI_LIST_AIRBAND_CHANNELS].clear();
    if (listening) {
        t.add(FOXAPI_LIST_AIRBAND_CHANNELS, 1, "Tower", "", FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_ON_AIR, 118.1e6, 10000, -40, 20);
        t.add(FOXAPI_LIST_AIRBAND_CHANNELS, 2, "Ground", "", FOXAPI_ITEM_ON_AIR, 121.9e6, 10000, -90, 0);
        t.add(FOXAPI_LIST_AIRBAND_CHANNELS, 3, "Weather ch1", "", 0, 156.8e6, 12500, -200, 0);
    }
}

void airbandReads() {
    StubTable idle;
    fillAirband(idle, false);
    FoxReceiverState s = blankState();
    AirbandModel m = readAirband(idle, s, ReadOptions{});
    // not listening: every ticked AM or NFM row (the WFM one and the unticked ATIS are not played), no lamps, no status lines
    CHECK(idsOf(m.rows) == (Ids{1, 2, 4, 6}) && m.count_line == "4 ticked of 4" && !m.listening && m.chip == "IDLE" && !m.lamp_lit);
    CHECK(m.hearing.empty() && m.block.empty() && m.listen_label == "LISTEN" && m.next_label.empty() && !m.above_noise_enabled);
    CHECK(m.rows.size() == 4 && !m.rows[0].heard && !m.rows[0].on_air && m.rows[0].tooltip.empty());
    CHECK(m.rows.size() == 4 && m.rows[0].text == "118.1000  Tower" && m.rows[0].heard_time == "1m 40s" && m.rows[1].heard_time == "1h 01m" && m.rows[2].heard_time == "45s" && m.rows[3].heard_time == "-");
    CHECK(m.lookup_label == "Look up" && m.nearest_label == "Nearest airports" && m.add_label == "Add" && m.import_label == "Import" && m.export_label == "Export CSV" && m.remove_preset_label == "Remove preset");
    CHECK(m.above_noise_label == "Above the noise" && m.squelch_text == "squelch -55 dB" && m.hold_text == "hold 2.0 s" && m.fact_note.empty());
    CHECK_NEAR(m.squelch_norm, 0.5, 1e-6);
    CHECK(m.groups.size() == 4 && m.groups[0].label == "Every ticked AM or NFM frequency" && m.groups[0].id.empty() && m.groups[1].id == "KORD" && m.groups[2].id == "Lugano" && m.groups[3].id == "Marine" && m.group_index == 0);
    CHECK(m.presets.size() == 3 && m.presets[0].id == "KORD" && m.preset == "Manual" && m.preset_index == -1 && m.preset_rows == 0);
    CHECK(m.tick_all_label.empty() && m.untick_all_label.empty());
    CHECK(m.modes.size() == 2 && m.modes[0].label == "AM" && m.modes[1].label == "NFM" && m.mode_index == 0);

    // listening: the lamps, the on-air flags, the status block and who is talking, the level tooltip, the pending seconds added to the row's own
    StubTable live;
    fillAirband(live, true);
    m = readAirband(live, s, ReadOptions{});
    CHECK(m.listening && m.chip == "LISTEN" && m.lamp_lit && m.listen_label == "STOP" && m.next_label == "Next block" && m.above_noise_enabled && m.blocks == 2);
    CHECK(m.block == "Block 1 of 2: 3 channels, 118.000-121.900 MHz" && m.hearing_line == "Hearing: Tower" && m.talking);
    CHECK(m.hearing == "Block 1 of 2: 3 channels, 118.000-121.900 MHz\nHearing: Tower");
    CHECK(idsOf(m.rows) == (Ids{1, 2, 4, 6}));
    CHECK(m.rows.size() == 4 && m.rows[0].heard && m.rows[0].on_air && !m.rows[1].heard && m.rows[1].on_air && !m.rows[2].heard && !m.rows[2].on_air && !m.rows[3].on_air);
    CHECK(m.rows.size() == 4 && m.rows[0].tooltip == "Level -40 dB (squelch -55 dB)" && m.rows[1].tooltip == "Level -90 dB (squelch -55 dB)" && m.rows[3].tooltip.empty());
    CHECK(m.rows.size() == 4 && m.rows[0].heard_time == "2m 00s");   // 100 s in the list + 20 s heard since the monitor last wrote it
    // two talking, and a quiet moment
    live.lists[FOXAPI_LIST_AIRBAND_CHANNELS][1].flags |= FOXAPI_ITEM_ACTIVE;
    m = readAirband(live, s, ReadOptions{});
    CHECK(m.hearing_line == "Hearing: Tower, Ground");
    live.lists[FOXAPI_LIST_AIRBAND_CHANNELS][0].flags = FOXAPI_ITEM_ON_AIR;
    live.lists[FOXAPI_LIST_AIRBAND_CHANNELS][1].flags = FOXAPI_ITEM_ON_AIR;
    m = readAirband(live, s, ReadOptions{});
    CHECK(m.hearing_line == "Hearing: -" && !m.talking && m.hearing == "Block 1 of 2: 3 channels, 118.000-121.900 MHz\nHearing: -");
    // one block only: no Next block
    live.facts["fact.airband.blocks"] = "1";
    CHECK(readAirband(live, s, ReadOptions{}).next_label.empty());
    live.facts["fact.airband.blocks"] = "2";
    // waiting for the patch to hand the radio back
    live.facts["fact.airband.listening"] = "0";
    live.facts["fact.airband.pending"] = "1";
    m = readAirband(live, s, ReadOptions{});
    CHECK(m.pending && m.hearing == "Handing the radio back from the patch view..." && m.listen_label == "STOP" && !m.listening && m.next_label == "Next block");
    live.facts["fact.airband.pending"] = "0";
    live.facts["fact.airband.listening"] = "1";

    // busiest first: by the time heard (the list's seconds and what its channel has heard since)
    ReadOptions b;
    b.air_busiest = true;
    m = readAirband(live, s, b);
    CHECK(idsOf(m.rows) == (Ids{2, 1, 4, 6}) && m.busiest_first);

    // a group on show: its AM and NFM rows, ticked or not; the preset is another thing
    ReadOptions g;
    g.air_group = "KORD";
    m = readAirband(idle, s, g);
    CHECK(idsOf(m.rows) == (Ids{1, 2, 3}) && m.count_line == "2 ticked of 3" && m.group == "KORD" && m.group_index == 1);
    CHECK(m.tick_all_label == "Tick all" && m.untick_all_label == "Untick all");
    g.air_group = "Gone";   // a group that has left the list is no longer on show
    m = readAirband(idle, s, g);
    CHECK(m.group.empty() && m.group_index == 0 && idsOf(m.rows) == (Ids{1, 2, 4, 6}) && m.tick_all_label.empty());
    ReadOptions pr;
    pr.air_preset = "  KORD ";
    m = readAirband(idle, s, pr);
    CHECK(m.preset == "KORD" && m.preset_rows == 3 && m.preset_index == 0 && m.export_enabled && m.remove_enabled && m.preset_name == "  KORD ");
    pr.air_preset = "";
    CHECK(readAirband(idle, s, pr).preset == "Manual");
    pr.air_preset = "Lugano";
    CHECK(readAirband(idle, s, pr).preset_index == 1 && readAirband(idle, s, pr).preset_rows == 1);
    pr.air_export_waiting = true;
    CHECK(!readAirband(idle, s, pr).export_enabled && readAirband(idle, s, pr).remove_enabled);
    pr.air_export_waiting = false;
    idle.facts["fact.bookmarks.exporting"] = "1";
    CHECK(!readAirband(idle, s, pr).export_enabled);
    idle.facts["fact.bookmarks.exporting"] = "0";

    // the drafts, the notes, the keys' states
    ReadOptions d;
    d.air_code = "KORD";
    d.air_freq = "121.5";
    d.air_name = "guard";
    d.air_import_path = "p.csv";
    d.air_mode = 1;
    d.air_note = "a note";
    d.air_preset_note = "Removed 3";
    d.air_folder_key = true;
    d.air_import_pending = true;
    d.air_import_elapsed_s = 3.2;
    m = readAirband(idle, s, d);
    CHECK(m.code_draft == "KORD" && m.freq_draft == "121.5" && m.name_draft == "guard" && m.import_path == "p.csv" && m.mode_index == 1 && m.note == "a note" && m.preset_note == "Removed 3");
    CHECK(m.lookup_enabled && m.add_enabled && !m.import_enabled && m.folder_label == "Open the folder" && m.disk_wait == "Waiting for the disk to open the file: 3 s");
    CHECK(!readAirband(idle, s, ReadOptions{}).lookup_enabled && !readAirband(idle, s, ReadOptions{}).add_enabled && readAirband(idle, s, ReadOptions{}).folder_label.empty());
    idle.facts["fact.airband.note"] = "Stopped: the receiver was retuned.";
    CHECK(readAirband(idle, s, ReadOptions{}).fact_note == "Stopped: the receiver was retuned.");

    // the monitor's settings: the engine's, or the hand's until the engine has caught up
    ReadOptions h;
    h.squelch_held = true;
    h.squelch_edit = -80.0f;
    h.hold_held = true;
    h.hold_edit = 7.5;
    m = readAirband(idle, s, h);
    CHECK(m.squelch_text == "squelch -80 dB" && !m.squelch_from_engine && m.hold_text == "hold 7.5 s" && m.hold_s == 7.5);
    CHECK_NEAR(m.squelch_norm, 30.0 / 110.0, 1e-6);
    idle.facts["fact.airband.squelchDb"] = "-110";
    CHECK_NEAR(readAirband(idle, s, ReadOptions{}).squelch_norm, 0.0, 1e-9);
    idle.facts["fact.airband.squelchDb"] = "0";
    CHECK_NEAR(readAirband(idle, s, ReadOptions{}).squelch_norm, 1.0, 1e-9);
    idle.facts["fact.airband.squelchDb"] = "-55.0";

    // the airports: the nearest list and the lookup's choices, as the rows print them
    core::Airport kord, egll, twin;
    kord.ident = "KORD";
    kord.name = "Chicago O'Hare International Airport";
    kord.country = "US";
    egll.ident = "EGLL";
    egll.name = "Heathrow";
    egll.country = "GB";
    twin.ident = "ORD";
    twin.name = "Some Field";
    twin.country = "DE";
    ReadOptions a;
    a.nearest.push_back({&kord, 33.6});
    a.nearest.push_back({&egll, 1234.4});
    a.choices.push_back(&twin);
    a.choices.push_back(&kord);
    m = readAirband(idle, s, a);
    CHECK(m.hide_label == "Hide" && m.airports.size() == 2 && m.airports[0].code == "KORD" && m.airports[0].distance == "34 km" && m.airports[1].distance == "1234 km");
    CHECK(m.ambiguous.size() == 2 && m.ambiguous[0].code == "ORD" && m.ambiguous[0].name == "Some Field" && m.ambiguous[0].note == "(DE)");
    CHECK(readAirband(idle, s, ReadOptions{}).hide_label.empty() && readAirband(idle, s, ReadOptions{}).airports.empty());
    CHECK(!readAirband(idle, s, ReadOptions{}).nearest_enabled && readAirband(idle, s, ReadOptions{}).nearest_disabled);
    s.flags |= FOXAPI_RX_POSITION_SET;
    CHECK(readAirband(idle, s, ReadOptions{}).nearest_enabled && !readAirband(idle, s, ReadOptions{}).nearest_disabled);
    // the keys that are dead: Export CSV with no rows in the preset ("Manual" holds none), a read that is out
    CHECK(readAirband(idle, s, ReadOptions{}).export_busy && !readAirband(idle, s, ReadOptions{}).import_busy);
    ReadOptions kordPreset;
    kordPreset.air_preset = "KORD";
    kordPreset.air_import_pending = true;
    CHECK(!readAirband(idle, s, kordPreset).export_busy && readAirband(idle, s, kordPreset).import_busy);
}

void markerReads() {
    StubTable t;
    t.facts["fact.markers.version"] = "4";
    t.facts["fact.markers.error"] = "markers.json is damaged";
    t.add(FOXAPI_LIST_FREQ_MARKERS, 1, "a long note cut", "", 0, 145.5e6, 1759241002);
    t.add(FOXAPI_LIST_FREQ_MARKERS, 2, "", "", 0, 7.07405e6, 1759241100);
    t.add(FOXAPI_LIST_FREQ_MARKERS, 3, "mid", "", 0, 100.0e6, 1759241200);
    t.facts["fact.markers.note.1"] = "a long note cut and then the rest of it";
    t.facts["fact.markers.note.3"] = "mid";
    ToolsCache cache;
    ReadOptions o;
    o.cache = &cache;
    o.markers_shown = true;
    MarkersModel m = readMarkers(t, o);
    CHECK(m.shown && m.error == "markers.json is damaged" && m.rows.size() == 3 && m.count == 3);
    // in frequency order, the whole note from its fact, the number as its name, the MHz in the fewest decimals
    CHECK(m.rows.size() == 3 && m.rows[0].number == 2 && m.rows[0].label == "M2" && m.rows[0].hz_text == "7.07405" && m.rows[0].note.empty());
    CHECK(m.rows.size() == 3 && m.rows[1].number == 3 && m.rows[1].hz_text == "100.000" && m.rows[1].note == "mid");
    CHECK(m.rows.size() == 3 && m.rows[2].number == 1 && m.rows[2].hz_text == "145.500" && m.rows[2].note == "a long note cut and then the rest of it" && m.rows[2].hz == 145.5e6);
    CHECK(m.rows.size() == 3 && m.rows[2].dropped == core::formatMarkerTime(1759241002) && m.rows[2].dropped.size() == 19 && m.rows[2].dropped_unix == 1759241002);
    CHECK(m.clear_label == "Clear all markers" && m.clear_confirm == "Yes, remove all 3" && m.clear_yes_label == m.clear_confirm && m.empty_line.empty());
    CHECK(m.copy_label == "Copy markers to clipboard");
    CHECK(!readMarkers(t, ReadOptions{}).shown);
    // the note being typed is the box's text until the list has it back
    o.note_hold.number = 3;
    o.note_hold.text = "mid-typed";
    o.note_hold.frames = 12;
    m = readMarkers(t, o);
    CHECK(m.rows.size() == 3 && m.rows[1].note == "mid-typed" && m.rows[0].note.empty());
    o.note_hold.frames = 0;
    { const MarkersModel back = readMarkers(t, o); CHECK(back.rows.size() == 3 && back.rows[1].note == "mid"); }
    // the rows are read again only when the list's own counter moves
    const int before = t.itemReads[FOXAPI_LIST_FREQ_MARKERS];
    readMarkers(t, o);
    CHECK(t.itemReads[FOXAPI_LIST_FREQ_MARKERS] == before && cache.markers.copies == 1);
    t.facts["fact.markers.version"] = "5";
    readMarkers(t, o);
    CHECK(t.itemReads[FOXAPI_LIST_FREQ_MARKERS] == before + 3 && cache.markers.copies == 2);
    // an empty list
    StubTable none;
    m = readMarkers(none, ReadOptions{});
    CHECK(m.rows.empty() && m.clear_label.empty() && m.copy_label.empty() && m.clear_yes_label.empty() && m.empty_line == "No markers yet. Right-click the waterfall and choose Drop marker here." && m.error.empty());
    // the clipboard text is the core's
    const MarkersModel full = readMarkers(t, o);
    const std::string clip = markersClipboard(full);
    CHECK(clip.rfind("FoxSDR markers (3)\n", 0) == 0 && has(clip, "M1") && has(clip, "a long note cut and then the rest of it"));
    CHECK(markersClipboard(MarkersModel{}).empty());
}

void smallRules() {
    CHECK(heardText(0.0) == "-" && heardText(0.9) == "-" && heardText(-5.0) == "-" && heardText(std::nan("")) == "-");
    CHECK(heardText(1.0) == "1s" && heardText(59.9) == "59s" && heardText(60.0) == "1m 00s" && heardText(750.0) == "12m 30s" && heardText(3599.0) == "59m 59s");
    CHECK(heardText(3600.0) == "1h 00m" && heardText(3725.0) == "1h 02m" && heardText(7385.0) == "2h 03m");
    CHECK(trimmedGroupName("  LUGANO \t") == "LUGANO" && trimmedGroupName(" \r\n ").empty() && trimmedGroupName("a b") == "a b" && trimmedGroupName("").empty());
    CHECK(coordinateText(51.5) == "51.50000" && coordinateText(-0.123456789) == "-0.12346");
    CHECK(offerPlace(51.5, -0.12) == "51.50N 0.12W" && knownPlace(-33.865, 151.209) == "33.86500S 151.20900E");
    CHECK(airbandPlays(FOXAPI_DEMOD_AM) && airbandPlays(FOXAPI_DEMOD_NFM) && !airbandPlays(FOXAPI_DEMOD_WFM) && !airbandPlays(0) && !airbandPlays(FOXAPI_DEMOD_USB));
    const auto fields = scannerFieldTexts(blankState());
    CHECK(fields[0] == "0.0000" && fields[2] == "0.00" && fields[6] == "0");
    // the rows an import sends: a tab or a break in a field is a space, the flags 1 ticked + 2 favourite
    core::Bookmark b;
    b.name = "a\tb\nc";
    b.group = "g\rh";
    b.mode = "NFM";
    b.freqHz = 156.8e6;
    b.bandwidthHz = 12500;
    b.favourite = true;
    CHECK(bookmarkRowLine(b, true) == "156800000.000000\tNFM\t12500.000\t3\tg h\ta b c\n");
    CHECK(bookmarkRowLine(b, false) == "156800000.000000\tNFM\t12500.000\t2\tg h\ta b c\n");
    b.favourite = false;
    CHECK(bookmarkRowLine(b, false) == "156800000.000000\tNFM\t12500.000\t0\tg h\ta b c\n");
    // a long list goes in chunks the engine takes (512 KiB at most, cut on a line end), none lost
    std::vector<core::Bookmark> many(9000, b);
    for (std::size_t i = 0; i < many.size(); ++i) { many[i].name = std::string(90, 'n') + std::to_string(i); }
    const std::vector<std::string> chunks = bookmarkRowChunks(many);
    std::string whole;
    bool sized = true, lined = true;
    for (const std::string& c : chunks) {
        sized = sized && c.size() <= 512u * 1024u;
        lined = lined && !c.empty() && c.back() == '\n';
        whole += c;
    }
    std::string expect;
    for (const core::Bookmark& m : many) { expect += bookmarkRowLine(m, m.scan); }
    CHECK(chunks.size() >= 2 && sized && lined && whole == expect);
    CHECK(bookmarkRowChunks({}).empty());
}

void toolsRead() {
    StubTable t;
    fillAirband(t, true);
    t.facts["fact.scanner.listMode"] = "0";
    t.facts["fact.markers.version"] = "1";
    FoxReceiverState s = blankState();
    s.flags |= FOXAPI_RX_SCANNER_ACTIVE;
    s.scannerState = FOXAPI_SCANNER_SCANNING;
    ToolsCache cache;
    ReadOptions o;
    o.cache = &cache;
    o.scope_signal = 3;
    o.scope_open = true;
    o.scope_live = true;
    const ToolsModels m = readTools(t, s, o);
    CHECK(m.chips[kRowDemodScope].word == "VECTOR" && m.chips[kRowDemodScope].lit);
    CHECK(m.chips[kRowRadar].word == "OFF" && !m.chips[kRowRadar].lit && m.chips[kRowRadar].tone == Tone::Neutral);
    CHECK(m.chips[kRowBookmarks].word == "7" && m.chips[kRowBookmarks].lit);
    CHECK(m.chips[kRowScanner].word == "SCAN" && m.chips[kRowScanner].lit);
    CHECK(m.chips[kRowAirband].word == "LISTEN" && m.chips[kRowAirband].lit);
    CHECK(m.bookmarks.rows.size() == 7 && m.airband.rows.size() == 4 && m.scanner.scanning && !m.radar.offers.known);
    // the bookmark list is copied ONCE for the bookmarks and the airband rows both
    CHECK(cache.bookmarks.copies == 1 && t.itemReads[FOXAPI_LIST_BOOKMARKS] == 7);
    // with no cache, a read is whole
    StubTable u;
    fillAirband(u, false);
    const ToolsModels n = readTools(u, blankState(), ReadOptions{});
    CHECK(n.chips[kRowBookmarks].word == "7" && n.chips[kRowAirband].word == "IDLE" && !n.chips[kRowScanner].lit && n.chips[kRowScanner].word == "IDLE");
}

// ================================================================================================================================
// the effects
// ================================================================================================================================

void positionEffects() {
    Effect e = applyScopeClicked();
    CHECK(e.local == Local::Deferred && e.text == "demod-scope" && e.commands.empty() && !e.empty());
    e = applyRadarScopeClicked();
    CHECK(e.local == Local::Deferred && e.text == "radar-scope" && e.commands.empty());

    ReadOptions o;
    o.traffic_heard = 4;
    o.traffic_lat = 51.5;
    o.traffic_lon = -0.12;
    e = applyPositionHeard(o);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SET_POSITION && fcOf(e, 0).num[0] == 51.5 && fcOf(e, 0).num[1] == -0.12 && cmdOf(e, 0).valid && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS && e.coverage_reset);
    o.traffic_heard = 2;
    CHECK(applyPositionHeard(o).empty());
    o.traffic_heard = 4;
    o.traffic_lat = 0.0;
    o.traffic_lon = 0.0;
    CHECK(applyPositionHeard(o).empty());

    ReadOptions m;
    m.map_have = true;
    m.map_lat = 53.1;
    m.map_lon = 8.2;
    e = applyPositionMap(m);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SET_POSITION && fcOf(e, 0).num[0] == 53.1 && fcOf(e, 0).num[1] == 8.2 && e.coverage_reset);
    m.map_lat = 0.0;
    m.map_lon = 0.0;
    CHECK(applyPositionMap(m).empty());
    m.map_lat = 53.1;
    m.map_have = false;
    CHECK(applyPositionMap(m).empty());

    CHECK(applyPositionLat("51.5").local == Local::PositionLat && applyPositionLat("51.5").text == "51.5" && applyPositionLat("51.5").commands.empty());
    CHECK(applyPositionLon("-0.1").local == Local::PositionLon && applyPositionLon("-0.1").text == "-0.1");

    e = applyPositionSet(" 51.5 ", "-0.1");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SET_POSITION && fcOf(e, 0).num[0] == 51.5 && fcOf(e, 0).num[1] == -0.1 && e.coverage_reset);
    e = applyPositionSet("51.5", "");   // an emptied box is 0, as InputDouble leaves it
    CHECK(e.commands.size() == 1 && fcOf(e, 0).num[0] == 51.5 && fcOf(e, 0).num[1] == 0.0);
    CHECK(applyPositionSet("0", "0").empty());          // the Gulf of Guinea is not a receiver
    CHECK(applyPositionSet("", "").empty());
    CHECK(applyPositionSet("91", "10").empty() && applyPositionSet("-91", "10").empty());
    CHECK(applyPositionSet("10", "181").empty() && applyPositionSet("10", "-180.5").empty());
    CHECK(applyPositionSet("abc", "10").empty() && applyPositionSet("10", "1x").empty());
    CHECK(!applyPositionSet("90", "180").empty() && !applyPositionSet("-90", "-180").empty());   // the edges are inside
    CHECK(applyPositionSet("nan", "10").empty() && applyPositionSet("inf", "10").empty());
    double v = 0.0;
    CHECK(parseCoordinate(" 12.5 ", v) && v == 12.5);
    CHECK(!parseCoordinate("", v) && !parseCoordinate("1,5", v) && !parseCoordinate("12.5.1", v) && v == 12.5);
}

void bookmarkEffects() {
    // Add current: ungrouped with the plain op, grouped with the group op (the 0x1F separator), the group trimmed, the name as typed
    Effect e = applyBookmarkAdd("Tower", "");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_ADD && commandText(cmdOf(e, 0)) == "Tower" && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    e = applyBookmarkAdd("", " \t ");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_ADD && commandText(cmdOf(e, 0)).empty());
    // an empty name with the tune known is named by the frequency, the list row's own figure (6.46 finding 1, docs 6.52); blanks count as empty; a group keeps its op
    e = applyBookmarkAdd("", "", 100300000.0);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_ADD && commandText(cmdOf(e, 0)) == "100.3000 MHz");
    e = applyBookmarkAdd(" \t", "Air", 118500000.0);
    CHECK(fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_GROUP && commandText(cmdOf(e, 0)) == std::string("Air\x1F") + "118.5000 MHz");
    CHECK(commandText(cmdOf(applyBookmarkAdd("Tower", "", 100300000.0), 0)) == "Tower");   // a typed name is never replaced
    e = applyBookmarkAdd("Tower", "  LUGANO ");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_GROUP && fcOf(e, 0).ival[0] == 0 && commandText(cmdOf(e, 0)) == std::string("LUGANO\x1F") + "Tower");
    e = applyBookmarkAdd("", "G");
    CHECK(commandText(cmdOf(e, 0)) == std::string("G\x1F"));
    // a name past the slot rides in the long text, whole
    const std::string longName(400, 'n');
    e = applyBookmarkAdd(longName, "");
    CHECK(commandText(cmdOf(e, 0)) == longName && !cmdOf(e, 0).q.longText.empty());
    CHECK(applyBookmarkName("x").local == Local::BookmarkName && applyBookmarkGroup("g").local == Local::BookmarkGroup && applyBookmarkImportPath("p").local == Local::BookmarkImportPath);
    CHECK(applyBookmarkSearch("s").local == Local::BookmarkSearch && applyBookmarkSearch("s").text == "s");

    // the import: nothing while one is out; empty (or only blanks) opens the window; a path with a root goes as it stands; any other is looked for on a worker
    BookmarksModel m = bookmarkModel();
    m.import_path = "";
    e = applyBookmarkImport(m);
    CHECK(e.local == Local::OpenImportDialog && !e.flag && e.commands.empty());
    m.import_path = " \r\n";
    CHECK(applyBookmarkImport(m).local == Local::OpenImportDialog);
    m.import_path = "\"C:/lists/marine.csv\"\r\n";
    e = applyBookmarkImport(m);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_IMPORT_FILE && commandText(cmdOf(e, 0)) == "C:/lists/marine.csv" && e.local == Local::None && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    m.import_path = "file:///C:/x/y%20z.csv";
    CHECK(commandText(cmdOf(applyBookmarkImport(m), 0)) == "C:/x/y z.csv");
    m.import_path = "//server/share/y.csv";
    CHECK(applyBookmarkImport(m).commands.size() == 1);
    m.import_path = "frequencies.xml";
    e = applyBookmarkImport(m);
    CHECK(e.commands.empty() && e.local == Local::ImportLookup && e.text == "frequencies.xml");
    m.import_path = "  lists/marine.csv ";
    CHECK(applyBookmarkImport(m).text == "lists/marine.csv");
    m.import_enabled = false;
    CHECK(applyBookmarkImport(m).empty());

    // the filter by index: "Every group" is "", each other is the group's name
    m = bookmarkModel();
    CHECK(applyBookmarkFilter(m, 0).local == Local::BookmarkFilter && applyBookmarkFilter(m, 0).text.empty());
    CHECK(applyBookmarkFilter(m, 1).text == "KORD" && applyBookmarkFilter(m, 3).text == "Marine");
    CHECK(applyBookmarkFilter(m, 4).empty() && applyBookmarkFilter(m, -1).empty());
    CHECK(applyBookmarkFavourites(true).local == Local::BookmarkFavourites && applyBookmarkFavourites(true).flag && !applyBookmarkFavourites(false).flag);
    CHECK(applyBookmarkOnSpectrum(false).local == Local::BookmarkOnSpectrum && !applyBookmarkOnSpectrum(false).flag);
    CHECK(applyBookmarkStack(m, false).local == Local::BookmarkStack && !applyBookmarkStack(m, false).flag && applyBookmarkStack(m, true).flag);
    ReadOptions off;
    off.on_spectrum = false;
    CHECK(applyBookmarkStack(bookmarkModel(off), true).empty());

    // the export: the ids of the rows SHOWN, each followed by a space
    e = applyBookmarkExport(m);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_EXPORT && commandText(cmdOf(e, 0)) == "1 2 3 4 5 6 7 " && fcOf(e, 0).ival[0] == 0);
    ReadOptions fav;
    fav.favourites_only = true;
    CHECK(commandText(cmdOf(applyBookmarkExport(bookmarkModel(fav)), 0)) == "2 5 ");
    ReadOptions nomatch;
    nomatch.search = "zzz";
    CHECK(applyBookmarkExport(bookmarkModel(nomatch)).empty());
    BookmarksModel busy = m;
    busy.export_enabled = false;
    CHECK(applyBookmarkExport(busy).empty());

    // remove this group: the op, the filter back to every group; nothing with no group chosen
    CHECK(applyBookmarkRemoveGroup(m).empty());
    ReadOptions g;
    g.filter_group = "KORD";
    g.rename_open = true;
    g.rename_draft = "Chicago";
    BookmarksModel gm = bookmarkModel(g);
    e = applyBookmarkRemoveGroup(gm);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_REMOVE_GROUP && commandText(cmdOf(e, 0)) == "KORD" && e.local == Local::BookmarkFilter && e.text.empty());

    // rename: the group op, action 2, "from 0x1F to"; the draft trimmed; nothing for an empty or an unchanged name
    e = applyBookmarkRename(gm);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_GROUP && fcOf(e, 0).ival[0] == 2 && commandText(cmdOf(e, 0)) == std::string("KORD\x1F") + "Chicago" && e.local == Local::BookmarkRenamed && e.text == "Chicago");
    BookmarksModel rn = gm;
    rn.rename_draft = "  Chicago  ";
    CHECK(commandText(cmdOf(applyBookmarkRename(rn), 0)) == std::string("KORD\x1F") + "Chicago");
    rn.rename_draft = "KORD";
    CHECK(applyBookmarkRename(rn).empty());
    rn.rename_draft = "  ";
    CHECK(applyBookmarkRename(rn).empty());
    CHECK(applyBookmarkRename(m).empty());   // no group chosen
    rn.rename_draft = "Marine";   // into a name the list has: the two become one (the engine merges)
    CHECK(commandText(cmdOf(applyBookmarkRename(rn), 0)) == std::string("KORD\x1F") + "Marine");
    CHECK(applyBookmarkRenameDraft("abc").local == Local::BookmarkRenameDraft && applyBookmarkRenameDraft("abc").text == "abc");
    e = applyBookmarkRenameOpen(gm);   // open and seeded with the group's name; the key toggles
    CHECK(e.local == Local::BookmarkRenameOpen && e.text == "KORD" && !e.flag);   // the model says it is open: the key closes it
    gm.rename_open = false;
    e = applyBookmarkRenameOpen(gm);
    CHECK(e.local == Local::BookmarkRenameOpen && e.flag && e.text == "KORD");
    CHECK(applyBookmarkRenameOpen(m).empty());

    // a row's controls: by id, nothing for an id the rows shown do not hold
    e = applyBookmarkTick(m, 4, true);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_TICK && fcOf(e, 0).ival[0] == 4 && fcOf(e, 0).ival[1] == 1 && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    CHECK(fcOf(applyBookmarkTick(m, 4, false), 0).ival[1] == 0);
    CHECK(applyBookmarkTick(m, 99, true).empty());
    e = applyBookmarkFavourite(m, 5, false);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_FAVOURITE && fcOf(e, 0).ival[0] == 5 && fcOf(e, 0).ival[1] == 0);
    CHECK(fcOf(applyBookmarkFavourite(m, 5, true), 0).ival[1] == 1 && applyBookmarkFavourite(m, 99, true).empty());
    e = applyBookmarkTune(m, 2);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_TUNE && fcOf(e, 0).ival[0] == 2 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    CHECK(applyBookmarkTune(m, 99).empty());
    e = applyBookmarkDelete(m, 6);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_REMOVE && fcOf(e, 0).ival[0] == 6 && applyBookmarkDelete(m, 99).empty());
    // a row filtered out of the view is not there to act on
    ReadOptions only;
    only.filter_group = "Marine";
    CHECK(applyBookmarkDelete(bookmarkModel(only), 1).empty() && !applyBookmarkDelete(bookmarkModel(only), 4).empty());

    // the row's menu and the move: by id, "" is no group, a row already there changes nothing, the menu closes either way
    CHECK(applyBookmarkMenu(3).local == Local::BookmarkMenu && applyBookmarkMenu(3).number == 3 && applyBookmarkMenu(-1).number == -1 && applyBookmarkMenu(3).commands.empty());
    e = applyBookmarkMove(m, 5, "KORD");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_GROUP && fcOf(e, 0).ival[0] == 1 && fcOf(e, 0).ival[1] == 5 && commandText(cmdOf(e, 0)) == "KORD" && e.local == Local::BookmarkMenu && e.number == -1);
    e = applyBookmarkMove(m, 1, "");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).ival[1] == 1 && commandText(cmdOf(e, 0)).empty());
    e = applyBookmarkMove(m, 5, "   ");   // already in no group
    CHECK(e.commands.empty() && e.local == Local::BookmarkMenu && e.number == -1);
    e = applyBookmarkMove(m, 1, "KORD");
    CHECK(e.commands.empty() && e.local == Local::BookmarkMenu);
    e = applyBookmarkMove(m, 5, "  New group ");
    CHECK(commandText(cmdOf(e, 0)) == "New group");
    CHECK(applyBookmarkMove(m, 99, "KORD").commands.empty());
}

void scannerEffects() {
    CHECK(fcOf(applyScannerTicked(true), 0).op == FOXAPP_OP_SCANNER_LIST && fcOf(applyScannerTicked(true), 0).ival[0] == 1 && cmdOf(applyScannerTicked(true), 0).grant == FOXAPI_GRANT_TUNE);
    CHECK(fcOf(applyScannerTicked(false), 0).ival[0] == 0);

    FoxReceiverState s = blankState();
    s.scanStartHz = 88.0e6;
    s.scanStopHz = 108.0e6;
    s.scanStepHz = 100.0e3;
    s.scanDwellMs = 150;
    s.scanHoldMs = 2000;
    s.scanResumeMs = 500;
    s.scanListenMs = 0;
    StubTable t;
    ScannerModel idle = readScanner(t, s, ReadOptions{});
    FoxReceiverState rs = s;
    rs.flags |= FOXAPI_RX_SCANNER_ACTIVE;
    ScannerModel running = readScanner(t, rs, ReadOptions{});

    // a range field: SCANNER_RANGE with its bit, the three values in Hz (the edited one from the text, the rest the engine's); +8 while the scan runs
    Effect e = applyScannerField(idle, s, kFieldStart, "144.5");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_SCANNER_RANGE && fcOf(e, 0).ival[0] == 1 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    CHECK(fcOf(e, 0).num[0] == 144.5e6 && fcOf(e, 0).num[1] == 108.0e6 && fcOf(e, 0).num[2] == 100.0e3 && e.local == Local::ScannerDraft && e.number == kFieldStart && e.text == "144.5");
    e = applyScannerField(idle, s, kFieldStop, "146");
    CHECK(fcOf(e, 0).op == FOXAPP_OP_SCANNER_RANGE && fcOf(e, 0).ival[0] == 2 && fcOf(e, 0).num[0] == 88.0e6 && fcOf(e, 0).num[1] == 146.0e6);
    e = applyScannerField(idle, s, kFieldStep, "12.5");
    CHECK(fcOf(e, 0).ival[0] == 4 && fcOf(e, 0).num[2] == 12500.0 && fcOf(e, 0).num[0] == 88.0e6);
    e = applyScannerField(running, rs, kFieldStart, "90");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_SCANNER_RANGE && fcOf(e, 0).ival[0] == (1 | 8) && fcOf(e, 0).num[0] == 90.0e6);
    CHECK(fcOf(applyScannerField(running, rs, kFieldStop, "91"), 0).ival[0] == (2 | 8) && fcOf(applyScannerField(running, rs, kFieldStep, "5"), 0).ival[0] == (4 | 8));

    // a timing field: SCANNER_CONFIG carries all four (the edited one from the text, the rest the engine's); it reconfigures a running scan by itself
    const struct { int field; double want[4]; } timings[] = {
        {kFieldDwell, {300, 2000, 500, 0}}, {kFieldHold, {150, 3000, 500, 0}}, {kFieldResume, {150, 2000, 800, 0}}, {kFieldListen, {150, 2000, 500, 9000}}};
    const char* const typed[] = {"300", "3000", "800", "9000"};
    for (int i = 0; i < 4; ++i) {
        for (int run = 0; run < 2; ++run) {
            e = applyScannerField(run != 0 ? running : idle, run != 0 ? rs : s, timings[i].field, typed[i]);
            CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SCANNER_CONFIG && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
            for (int k = 0; k < 4; ++k) { CHECK(fcOf(e, 0).num[k] == timings[i].want[k]); }
            CHECK(e.number == timings[i].field && e.local == Local::ScannerDraft);
        }
    }
    // text that is no number, or empty, changes nothing; a field out of range is none
    CHECK(applyScannerField(idle, s, kFieldStart, "abc").empty() && applyScannerField(idle, s, kFieldStart, "").empty() && applyScannerField(idle, s, kFieldStart, "1x").empty());
    CHECK(applyScannerField(idle, s, 7, "1").empty() && applyScannerField(idle, s, -1, "1").empty());
    CHECK(applyScannerField(idle, s, kFieldDwell, "inf").empty());

    // Start: the range the engine stores, or - in list mode - the ticked rows (nothing ticked: nothing)
    e = applyScannerRun(idle, s);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SCANNER_RUN && fcOf(e, 0).ival[0] == 1 && fcOf(e, 0).num[0] == 88.0e6 && fcOf(e, 0).num[1] == 108.0e6 && fcOf(e, 0).num[2] == 100.0e3 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    ScannerModel list = idle;
    list.ticked_mode = true;
    list.ticked_count = 3;
    e = applyScannerRun(list, s);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_SCANNER_LIST && fcOf(e, 0).ival[0] == 2 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    list.ticked_count = 0;
    CHECK(applyScannerRun(list, s).empty());
    CHECK(applyScannerRun(running, rs).empty());   // while it runs the key is Stop and Skip

    e = applyScannerHalt(running);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SCANNER_RUN && fcOf(e, 0).ival[0] == 0 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    e = applyScannerSkip(running);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SCANNER_SKIP && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    CHECK(applyScannerHalt(idle).empty() && applyScannerSkip(idle).empty());
}

// The real table's airports: the one a lookup finds, and a code that is two airports' at once.
const core::Airport* findOne(const std::string& code) {
    const std::vector<const core::Airport*> f = core::findAirports(core::airbandTable(), code);
    return f.size() == 1 ? f[0] : nullptr;
}

std::string ambiguousCode() {
    std::unordered_map<std::string, int> count;
    const auto up = [](std::string s) {
        for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
        return s;
    };
    for (const core::Airport& a : core::airbandTable()) {
        std::set<std::string> mine;
        for (const std::string& c : {a.ident, a.iata, a.local}) {
            if (!c.empty()) { mine.insert(up(c)); }
        }
        for (const std::string& c : mine) { ++count[c]; }
    }
    for (const auto& kv : count) {
        if (kv.second >= 2) { return kv.first; }
    }
    return std::string();
}

void airbandEffects() {
    StubTable idleT;
    fillAirband(idleT, false);
    StubTable liveT;
    fillAirband(liveT, true);
    FoxReceiverState s = blankState();
    const AirbandModel idle = readAirband(idleT, s, ReadOptions{});
    const AirbandModel live = readAirband(liveT, s, ReadOptions{});

    CHECK(applyAirbandCode("kord").local == Local::AirbandCode && applyAirbandCode("kord").text == "KORD" && applyAirbandCode("e gll").text == "E GLL");

    // the lookup: nothing with no code; no such airport says so; one airport is a LIST of rows; several are choices
    CHECK(applyAirbandLookup("").empty());
    Effect e = applyAirbandLookup("ZZZZZZZZ");
    CHECK(e.set_note && e.note == "No airport with the code \"ZZZZZZZZ\" publishes an airband frequency in FoxSDR's table." && e.commands.empty() && e.clear_choices && !e.set_choices);
    const core::Airport* kord = findOne("KORD");
    CHECK(kord != nullptr);
    if (kord != nullptr) {
        e = applyAirbandLookup("KORD");
        const std::vector<core::Bookmark> rows = core::airportBookmarks(*kord);
        CHECK(!rows.empty());
        CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_ADD_ROWS && fcOf(e, 0).ival[0] == 1 && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS && e.clear_choices);
        const std::string text = commandText(cmdOf(e, 0));
        CHECK(static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) == rows.size());
        std::size_t ticked = 0;
        for (const core::Bookmark& b : rows) { ticked += b.scan ? 1u : 0u; }
        CHECK(e.ask.kind == AskKind::AirportRows && e.ask.group == core::airportGroupName(*kord) && e.ask.total == rows.size() && e.ask.ticked == ticked && ticked > 0 && ticked <= rows.size());
        // a row: "<Hz>\t<mode>\t<bandwidth>\t<flags>\t<group>\t<name>", the flags 1 for a ticked one, the group the airport's
        const std::size_t eol = text.find('\n');
        const std::string first = text.substr(0, eol);
        CHECK(std::count(first.begin(), first.end(), '\t') == 5 && first.find('\t') != std::string::npos);
        CHECK(first == bookmarkRowLine(rows[0], rows[0].scan).substr(0, first.size()) && has(first, core::airportGroupName(*kord).c_str()));
        CHECK(!has(text, "\r") && fcOf(e, 0).ival[1] == 0);
        // ATIS and weather broadcasts are added unticked: a row whose flags field is 0 exists whenever the airport lists one
        bool anyUnticked = false;
        for (const core::Bookmark& b : rows) { anyUnticked = anyUnticked || !b.scan; }
        CHECK(anyUnticked == (ticked < rows.size()));
    }
    const std::string twin = ambiguousCode();
    CHECK(!twin.empty());
    if (!twin.empty()) {
        const std::vector<const core::Airport*> found = core::findAirports(core::airbandTable(), twin);
        e = applyAirbandLookup(twin);
        CHECK(e.commands.empty() && e.set_choices && e.choices == found && e.clear_choices);
        CHECK(e.note == core::formatText(tr("\"%s\" names %zu airports - pick one:"), twin.c_str(), found.size()) && has(e.note, "airports - pick one:"));
    }

    // the nearest: the six airports around the receiver, nearest first; nothing without a position
    CHECK(applyAirbandNearest(s).empty());
    FoxReceiverState at = s;
    at.flags |= FOXAPI_RX_POSITION_SET;
    at.rxLatDeg = 41.9786;
    at.rxLonDeg = -87.9048;
    e = applyAirbandNearest(at);
    CHECK(e.set_nearest && e.nearest.size() == 6 && e.clear_choices && e.commands.empty());
    bool ascending = true;
    for (std::size_t i = 1; i < e.nearest.size(); ++i) { ascending = ascending && e.nearest[i - 1].second <= e.nearest[i].second; }
    CHECK(ascending && !e.nearest.empty() && e.nearest[0].second < 15.0 && e.nearest[0].first != nullptr);
    e = applyAirbandHide();
    CHECK(e.clear_nearest && !e.set_nearest && e.commands.empty() && !e.empty());

    // picking: i < 1000 is a nearest row (the nearest list goes), 1000 + i a choice
    ReadOptions pick;
    pick.nearest = applyAirbandNearest(at).nearest;
    pick.choices = core::findAirports(core::airbandTable(), twin.empty() ? std::string("KORD") : twin);
    e = applyAirbandAirport(pick, 2);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_ADD_ROWS && fcOf(e, 0).ival[0] == 1 && e.clear_nearest && e.clear_choices && e.ask.kind == AskKind::AirportRows);
    CHECK(pick.nearest.size() > 2 && e.ask.group == core::airportGroupName(*pick.nearest[2].first));
    e = applyAirbandAirport(pick, kChoiceBase + 0);
    CHECK(e.commands.size() == 1 && !e.clear_nearest && e.clear_choices && !pick.choices.empty() && e.ask.group == core::airportGroupName(*pick.choices[0]));
    CHECK(applyAirbandAirport(pick, 6).empty() && applyAirbandAirport(pick, -1).empty() && applyAirbandAirport(pick, kChoiceBase + 99).empty() && applyAirbandAirport(ReadOptions{}, 0).empty());

    // the preset field: a name, or one chosen from the list (trimmed; one that does not fit gives "")
    CHECK(applyAirbandPresetName("My list").local == Local::AirbandPresetName && applyAirbandPresetName("My list").text == "My list");
    e = applyAirbandPreset(idle, 0);
    CHECK(e.local == Local::AirbandPresetName && e.text == "KORD" && e.commands.empty());
    AirbandModel wide = idle;
    wide.presets.push_back(SelectItem{"  padded  ", "  padded  "});
    CHECK(applyAirbandPreset(wide, static_cast<int>(wide.presets.size()) - 1).text == "padded");
    wide.presets.push_back(SelectItem{std::string(200, 'x'), std::string(200, 'x')});
    CHECK(applyAirbandPreset(wide, static_cast<int>(wide.presets.size()) - 1).text.empty() && applyAirbandPreset(wide, static_cast<int>(wide.presets.size()) - 1).local == Local::AirbandPresetName);
    CHECK(applyAirbandPreset(idle, 9).empty() && applyAirbandPreset(idle, -1).empty());
    CHECK(applyAirbandFreq("121.5").local == Local::AirbandFreq && applyAirbandName("n").local == Local::AirbandName && applyAirbandMode(1).local == Local::AirbandMode && applyAirbandMode(1).number == 1 && applyAirbandMode(7).number == 0);
    CHECK(applyAirbandImportPath("p").local == Local::AirbandImportPath);

    // Add: the frequency, the mode, the name, the preset's group, ticked; the typed row's answer is waited for
    AirbandModel add = idle;
    add.freq_draft = "121.5";
    add.name_draft = "guard";
    add.mode_index = 0;
    e = applyAirbandAdd(add);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_ADD_ROWS && fcOf(e, 0).ival[0] == 0 && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    CHECK(commandText(cmdOf(e, 0)) == core::formatText("%.6f\tAM\t%.3f\t1\tManual\tguard\n", 121.5e6, core::defaultBandwidthForMode("AM")));
    CHECK(e.ask.kind == AskKind::TypedRow && e.ask.group == "Manual" && e.ask.total == 1 && e.ask.ticked == 1);
    add.mode_index = 1;
    add.name_draft = "";
    add.preset = "Marine";
    add.freq_draft = " 156,8 ";
    e = applyAirbandAdd(add);
    CHECK(commandText(cmdOf(e, 0)) == core::formatText("%.6f\tNFM\t%.3f\t1\tMarine\t\n", 156.8e6, core::defaultBandwidthForMode("NFM")) && e.ask.group == "Marine");
    add.name_draft = "tab\there\nnow";   // a tab or a break in a name is a space: it would split the row
    CHECK(commandText(cmdOf(applyAirbandAdd(add), 0)).find("\ttab here now\n") != std::string::npos);
    add.freq_draft = "121.5x";
    e = applyAirbandAdd(add);
    CHECK(e.commands.empty() && e.set_note && e.note == "could not read frequency \"121.5x\"");
    add.freq_draft = "";
    CHECK(applyAirbandAdd(add).empty());
    add.freq_draft = "0";
    CHECK(applyAirbandAdd(add).commands.empty() && applyAirbandAdd(add).set_note);

    // import: nothing while a read is out; an empty box opens the window for the preset; else the worker is the publisher's
    AirbandModel imp = idle;
    imp.import_path = "";
    imp.preset = "Marine";
    e = applyAirbandImport(imp);
    CHECK(e.local == Local::OpenImportDialog && e.flag && e.text == "Marine" && e.commands.empty());
    imp.import_path = "\"C:/x/list.csv\" ";
    e = applyAirbandImport(imp);
    CHECK(e.local == Local::PresetImportLookup && e.text == "C:/x/list.csv" && e.text2 == "Marine" && e.clear_preset_note && e.clear_folder_key && e.commands.empty());
    imp.import_enabled = false;
    CHECK(applyAirbandImport(imp).empty());
    // a path typed in the dialog that was opened for the preset (docs 6.61): the same lookup, for the preset the dialog names; nothing without a path or a preset
    e = applyAirbandImportInto("\"C:/x/list.csv\" ", "Marine");
    CHECK(e.local == Local::PresetImportLookup && e.text == "C:/x/list.csv" && e.text2 == "Marine" && e.clear_preset_note && e.clear_folder_key && e.commands.empty());
    CHECK(applyAirbandImportInto("", "Marine").empty() && applyAirbandImportInto("   ", "Marine").empty() && applyAirbandImportInto("C:/x/list.csv", "").empty());

    // export and remove: the preset's name, the answer waited for, the folder key forgotten; not without rows or while one is being written
    AirbandModel ex = idle;
    ex.preset = "KORD";
    ex.export_enabled = true;
    ex.remove_enabled = true;
    e = applyAirbandExport(ex);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_EXPORT && fcOf(e, 0).ival[0] == 1 && commandText(cmdOf(e, 0)) == "KORD" && e.ask.kind == AskKind::ExportPreset && e.ask.group == "KORD" && e.clear_folder_key);
    e = applyAirbandRemovePreset(ex);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_BOOKMARK_REMOVE_GROUP && commandText(cmdOf(e, 0)) == "KORD" && e.ask.kind == AskKind::RemoveGroup && e.clear_folder_key);
    ex.export_enabled = false;
    ex.remove_enabled = false;
    CHECK(applyAirbandExport(ex).empty() && applyAirbandRemovePreset(ex).empty());
    CHECK(applyAirbandExport(readAirband(idleT, s, ReadOptions{})).empty());   // the "Manual" preset has no rows: nothing to export
    ReadOptions kordPreset;
    kordPreset.air_preset = "KORD";
    CHECK(!applyAirbandExport(readAirband(idleT, s, kordPreset)).empty() && !applyAirbandRemovePreset(readAirband(idleT, s, kordPreset)).empty());

    // the folder: where the recordings are
    idleT.facts["fact.recorder.dir"] = "C:/Users/me/Recordings";
    e = applyAirbandFolder(idleT);
    CHECK(e.local == Local::OpenFolder && e.text == "C:/Users/me/Recordings");

    // choosing the group on show: the preset field takes its name; a monitor that listens starts again on it
    e = applyAirbandGroup(idle, 1);
    CHECK(e.local == Local::AirbandGroup && e.text == "KORD" && e.flag && e.text2 == "KORD" && e.commands.empty());
    e = applyAirbandGroup(idle, 0);
    CHECK(e.local == Local::AirbandGroup && e.text.empty() && !e.flag && e.commands.empty());
    e = applyAirbandGroup(live, 1);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 1 && commandText(cmdOf(e, 0)) == "KORD" && e.ask.kind == AskKind::Listen && e.ask.group == "KORD" && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    ReadOptions onKord;
    onKord.air_group = "KORD";
    const AirbandModel liveKord = readAirband(liveT, s, onKord);
    CHECK(applyAirbandGroup(liveKord, 1).commands.empty() && applyAirbandGroup(liveKord, 1).local == Local::AirbandGroup);   // the same group: not restarted
    CHECK(applyAirbandGroup(liveKord, 0).commands.size() == 1 && commandText(cmdOf(applyAirbandGroup(liveKord, 0), 0)).empty());
    AirbandModel waiting = idle;
    waiting.pending = true;
    CHECK(applyAirbandGroup(waiting, 2).commands.size() == 1);
    CHECK(applyAirbandGroup(idle, 9).empty());
    wide = idle;
    wide.groups.push_back(SelectItem{"long", std::string(200, 'g')});
    e = applyAirbandGroup(wide, static_cast<int>(wide.groups.size()) - 1);
    CHECK(e.flag && e.text2.empty() && e.text.size() == 200);   // a name that does not fit clears the preset field

    // tick all / untick all: the rows that differ (every one while a run is out); only with a group on show and rows listed
    CHECK(applyAirbandTickAll(idle, true, false).empty());   // no group on show
    ReadOptions grp;
    grp.air_group = "KORD";
    const AirbandModel kordRows = readAirband(idleT, s, grp);
    e = applyAirbandTickAll(kordRows, true, false);
    CHECK(e.local == Local::TickRun && e.flag && e.ids == (std::vector<std::uint64_t>{3}) && e.commands.empty());
    e = applyAirbandTickAll(kordRows, false, false);
    CHECK(e.local == Local::TickRun && !e.flag && e.ids == (std::vector<std::uint64_t>{1, 2}));
    e = applyAirbandTickAll(kordRows, true, true);
    CHECK(e.ids == (std::vector<std::uint64_t>{1, 2, 3}));
    AirbandModel emptyGroup = kordRows;
    emptyGroup.rows.clear();
    CHECK(applyAirbandTickAll(emptyGroup, true, false).empty());
    AirbandModel allTicked = kordRows;
    for (AirbandRow& r : allTicked.rows) { r.ticked = true; }
    e = applyAirbandTickAll(allTicked, true, false);   // already as asked: the run is replaced by nothing, but it IS said (an older run is cancelled)
    CHECK(e.local == Local::TickRun && e.ids.empty() && e.flag);
    CHECK(applyAirbandBusiest(true).local == Local::AirbandBusiest && applyAirbandBusiest(true).flag && !applyAirbandBusiest(false).flag);

    // a row's tick and click
    e = applyAirbandTick(4, true);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_TICK && fcOf(e, 0).ival[0] == 4 && fcOf(e, 0).ival[1] == 1 && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    CHECK(fcOf(applyAirbandTick(4, false), 0).ival[1] == 0);
    e = applyAirbandTune(6);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 6 && fcOf(e, 0).ival[1] == 6 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);

    // the squelch: dB from the slider's 0..1, sent on release, held until the engine's figure catches up
    e = applyAirbandSquelch(0.5f);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 4 && fcOf(e, 0).num[0] == -55.0 && e.local == Local::SquelchHeld && e.value == -55.0 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    CHECK(fcOf(applyAirbandSquelch(0.0f), 0).num[0] == -110.0 && fcOf(applyAirbandSquelch(1.0f), 0).num[0] == 0.0);
    CHECK(fcOf(applyAirbandSquelch(-3.0f), 0).num[0] == -110.0 && fcOf(applyAirbandSquelch(7.0f), 0).num[0] == 0.0);
    CHECK(applyAirbandSquelch(std::nanf("")).empty());

    // above the noise: only while it listens
    e = applyAirbandAboveNoise(live);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 3);
    CHECK(applyAirbandAboveNoise(idle).empty());

    // hold: the first number in the text, clamped to 0..60
    e = applyAirbandHold("hold 7.5 s");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 5 && fcOf(e, 0).num[0] == 7.5 && e.local == Local::HoldHeld && e.value == 7.5);
    CHECK(fcOf(applyAirbandHold("3"), 0).num[0] == 3.0 && fcOf(applyAirbandHold("99"), 0).num[0] == 60.0 && fcOf(applyAirbandHold("-4"), 0).num[0] == 0.0 && fcOf(applyAirbandHold("0"), 0).num[0] == 0.0);
    CHECK(applyAirbandHold("abc").empty() && applyAirbandHold("").empty() && applyAirbandHold("hold  s").empty());

    // LISTEN / STOP and Next block
    e = applyAirbandListen(idle);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 1 && commandText(cmdOf(e, 0)).empty() && e.ask.kind == AskKind::Listen && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    e = applyAirbandListen(kordRows);
    CHECK(commandText(cmdOf(e, 0)) == "KORD" && e.ask.group == "KORD");
    e = applyAirbandListen(live);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 0 && e.ask.kind == AskKind::None);
    CHECK(fcOf(applyAirbandListen(waiting), 0).ival[0] == 0);
    e = applyAirbandNext(live);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_AIRBAND && fcOf(e, 0).ival[0] == 2);
    AirbandModel one = live;
    one.blocks = 1;
    CHECK(applyAirbandNext(one).empty() && applyAirbandNext(idle).empty());
}

void markerEffects() {
    CHECK(applyMarkersOpen().local == Local::MarkersOpen && applyMarkersClosed().local == Local::MarkersClosed);
    Effect e = applyMarkerNote(3, "a note");
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_MARKER_NOTE && fcOf(e, 0).ival[0] == 3 && commandText(cmdOf(e, 0)) == "a note" && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    CHECK(e.local == Local::MarkerNoteHold && e.number == 3 && e.text == "a note");
    const std::string longNote(300, 'z');
    e = applyMarkerNote(2, longNote);
    CHECK(commandText(cmdOf(e, 0)) == longNote && cmdOf(e, 0).q.longText == longNote && e.text == longNote);
    e = applyMarkerNote(2, "");   // clears it
    CHECK(e.commands.size() == 1 && commandText(cmdOf(e, 0)).empty() && fcOf(e, 0).ival[0] == 2);
    e = applyMarkerRemove(5);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_MARKER_REMOVE && fcOf(e, 0).ival[0] == 5);

    StubTable t;
    t.facts["fact.markers.version"] = "1";
    t.add(FOXAPI_LIST_FREQ_MARKERS, 1, "", "", 0, 145.5e6, 1759241002);
    t.add(FOXAPI_LIST_FREQ_MARKERS, 2, "", "", 0, 7.07405e6, 1759241100);
    t.facts["fact.markers.note.2"] = "FT8\tcalling";
    const MarkersModel m = readMarkers(t, ReadOptions{});
    e = applyMarkersClear(m);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_MARKER_CLEAR && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    CHECK(applyMarkersClear(MarkersModel{}).empty());
    e = applyMarkerTune(m, 1);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPI_OP_SET_FREQUENCY && fcOf(e, 0).num[0] == 145.5e6 && cmdOf(e, 0).grant == FOXAPI_GRANT_TUNE);
    CHECK(applyMarkerTune(m, 9).empty());

    // the marker as a bookmark: the note (cleaned) or its number names it, the current mode and bandwidth, a LIST row (no range judged)
    FoxReceiverState s = blankState();
    s.demodMode = FOXAPI_DEMOD_USB;
    s.bandwidthHz = 2699.6;
    e = applyMarkerBookmark(m, 2, s);
    CHECK(e.commands.size() == 1 && fcOf(e, 0).op == FOXAPP_OP_BOOKMARK_ADD_ROWS && fcOf(e, 0).ival[0] == 1 && commandText(cmdOf(e, 0)) == "7074050.000000\tUSB\t2700\t0\t\tFT8 calling");
    e = applyMarkerBookmark(m, 1, s);
    CHECK(commandText(cmdOf(e, 0)) == "145500000.000000\tUSB\t2700\t0\t\tM1");
    s.demodMode = 0;
    CHECK(commandText(cmdOf(applyMarkerBookmark(m, 1, s), 0)) == "145500000.000000\tAM\t2700\t0\t\tM1");
    CHECK(applyMarkerBookmark(m, 9, s).empty());

    // Copy markers to clipboard: the core text, in frequency order; nothing for an empty list
    e = applyMarkersCopy(m);
    CHECK(e.local == Local::Clipboard && e.commands.empty() && e.text.rfind("FoxSDR markers (2)\n", 0) == 0 && e.text == markersClipboard(m));
    CHECK(e.text.find("M2") < e.text.find("M1") && has(e.text, "FT8 calling"));   // 7.07405 MHz is before 145.5 MHz
    CHECK(applyMarkersCopy(MarkersModel{}).empty());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
    sentencesAreTheDrawers();
    everySentenceIsADrawers();
    sentencesAreTranslated();
    scopeAndRadarReads();
    bookmarkReads();
    scannerReads();
    airbandReads();
    markerReads();
    toolsRead();
    smallRules();
    positionEffects();
    bookmarkEffects();
    scannerEffects();
    airbandEffects();
    markerEffects();
    return testSummary("test_slint_host_tools");
}
