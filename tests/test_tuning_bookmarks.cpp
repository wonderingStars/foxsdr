// The bookmark-side models of the tuning lane: CSV text, bookmark notes and tags, favourites that save the whole state, marker export and import,
// the station-database overlay, and bookmark / preset packs. Every parser of outside bytes has a fuzz run at the end.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/bookmark_notes.hpp"
#include "core/bookmark_pack.hpp"
#include "core/csv_overlay.hpp"
#include "core/csv_text.hpp"
#include "core/favourite_state.hpp"
#include "core/freq_markers.hpp"
#include "core/marker_export.hpp"
#include "test_check.hpp"

namespace ct = cascade::core::tuning;
namespace cc = cascade::core;

namespace {

struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    std::uint32_t u32() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<std::uint32_t>(s >> 32); }
};

using Rows = std::vector<std::vector<std::string>>;

// ---------------------------------------------------------------------------------------------------------------------------------------------
void testCsvRead() {
    Rows r;
    std::string err;
    ct::CsvLimits lim;
    CHECK(ct::parseCsv("a,b,c\r\n1,2,3\n4,5,6", r, lim, err));
    CHECK(r == (Rows{{"a", "b", "c"}, {"1", "2", "3"}, {"4", "5", "6"}}));
    // quoted: the delimiter, a doubled quote, a line break inside
    CHECK(ct::parseCsv("x,\"a,b\",\"say \"\"hi\"\"\",\"line1\nline2\"\n", r, lim, err));
    CHECK(r == (Rows{{"x", "a,b", "say \"hi\"", "line1\nline2"}}));
    // BOM dropped, empty lines skipped, empty fields kept
    CHECK(ct::parseCsv("\xEF\xBB\xBF" "a,,c\n\n\n,,\n", r, lim, err));
    CHECK(r == (Rows{{"a", "", "c"}, {"", "", ""}}));
    CHECK(ct::parseCsv("", r, lim, err) && r.empty());
    CHECK(ct::parseCsv("\n\n", r, lim, err) && r.empty());
    CHECK(ct::parseCsv("\"\"\n", r, lim, err) && r == (Rows{{""}}));   // a quoted empty field is a record
    // other delimiters
    CHECK(ct::parseCsv("a;b;\"c;d\"\n", r, lim, err, ';') && r == (Rows{{"a", "b", "c;d"}}));
    CHECK(ct::parseCsv("a\tb\n", r, lim, err, '\t') && r == (Rows{{"a", "b"}}));
    CHECK(!ct::parseCsv("a,b", r, lim, err, '"'));
    // refusals, each with a line number, and no rows left behind
    CHECK(!ct::parseCsv(std::string("a,b\nc,\0d\n", 9), r, lim, err) && err.find("line 2") == 0 && r.empty());
    CHECK(!ct::parseCsv("a,b\nc\"d,e\n", r, lim, err) && err.find("line 2") == 0);
    CHECK(!ct::parseCsv("a,\"b\"c\n", r, lim, err) && err.find("line 1") == 0);
    CHECK(!ct::parseCsv("a,\"b\n", r, lim, err) && err.find("never closed") != std::string::npos);
    CHECK(!ct::parseCsv("a,b\nc,\"d\"e\n", r, lim, err));
    // limits (each exactly at the edge and one over)
    ct::CsvLimits small;
    small.maxCols = 3; small.maxRows = 2; small.maxCell = 4; small.maxBytes = 30;
    CHECK(ct::parseCsv("a,b,c\nd,e,f\n", r, small, err));
    CHECK(!ct::parseCsv("a,b,c,d\n", r, small, err));
    CHECK(!ct::parseCsv("a,b,c\nd,e,f\ng,h,i\n", r, small, err) && err.find("too many rows") != std::string::npos);
    CHECK(ct::parseCsv("abcd\n", r, small, err));
    CHECK(!ct::parseCsv("abcde\n", r, small, err));
    CHECK(!ct::parseCsv("\"abcde\"\n", r, small, err));
    CHECK(!ct::parseCsv("abcd,abcd,abcd,abcd,abcd,abcd,abcd,abcd\n", r, small, err));
    CHECK(!ct::parseCsv(std::string(31, 'a'), r, small, err));
}

void testCsvWrite() {
    CHECK(ct::csvField("plain") == "plain");
    CHECK(ct::csvField("a,b") == "\"a,b\"");
    CHECK(ct::csvField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(ct::csvField("x\ny") == "\"x\ny\"");
    CHECK(ct::csvField("=1+1") == "'=1+1");
    CHECK(ct::csvField("+1") == "'+1");
    CHECK(ct::csvField("-1") == "'-1");
    CHECK(ct::csvField("@sum") == "'@sum");
    CHECK(ct::csvField("=1+1", false) == "=1+1");
    CHECK(ct::csvField("=a,b") == "\"'=a,b\"");
    CHECK(ct::csvField("") == "");
    CHECK(ct::csvField("a;b", true, ';') == "\"a;b\"");
    CHECK(ct::csvUnguard("'=1+1") == "=1+1");
    CHECK(ct::csvUnguard("'plain") == "'plain");
    CHECK(ct::csvUnguard("=1") == "=1");
    CHECK(ct::csvUnguard("") == "");
    // property: any field text written with the guard off reads back exactly
    Lcg rng(5);
    const char alpha[] = "ab ,\"\n\r;'=+-@\t\xC3\xA9z09";
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::string> row;
        const int n = 1 + static_cast<int>(rng.u32() % 4);
        for (int k = 0; k < n; ++k) {
            std::string f;
            const int len = static_cast<int>(rng.u32() % 8);
            for (int c = 0; c < len; ++c) { f.push_back(alpha[rng.u32() % (sizeof alpha - 1)]); }
            row.push_back(f);
        }
        if (row.size() == 1 && row[0].empty()) { row[0] = "x"; }   // a lone empty field is an empty line
        std::string line;
        for (std::size_t k = 0; k < row.size(); ++k) { line += (k ? "," : "") + ct::csvField(row[k], false); }
        Rows got;
        std::string err;
        const bool ok = ct::parseCsv(line + "\r\n", got, ct::CsvLimits(), err);
        CHECK(ok);
        CHECK(got == (Rows{row}));
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
void testNotes() {
    ct::BookmarkNotes n;
    std::string err;
    CHECK(n.set(145.5e6, "NFM", "Local repeater, strong at night\nsecond line", {"Repeater", " VHF ", "repeater", ""}, &err));
    const ct::BookmarkNote* f = n.find(145.5e6 + 0.3, "NFM");     // rounds to the same hertz
    CHECK(f != nullptr);
    if (f) {
        CHECK(f->note == "Local repeater, strong at night\nsecond line");
        CHECK(f->tags == (std::vector<std::string>{"repeater", "vhf"}));   // folded, trimmed, de-duplicated, blank dropped
    }
    CHECK(n.find(145.5e6, "AM") == nullptr);
    CHECK(n.find(145.5e6 + 1.0, "NFM") == nullptr);
    CHECK(n.size() == 1);
    // search and tags
    CHECK(n.set(7.074e6, "USB", "FT8 calling", {"digital"}));
    CHECK(n.byTag("VHF").size() == 1);
    CHECK(n.byTag("digital").size() == 1);
    CHECK(n.byTag("nothing").empty());
    CHECK(n.byTag("a,b").empty());
    CHECK(n.search("NIGHT").size() == 1);
    CHECK(n.search("ft8").size() == 1);
    CHECK(n.search("DIGI").size() == 1);
    CHECK(n.search("").empty());
    CHECK(n.search("zzz").empty());
    // empty note and no tags removes
    CHECK(n.set(7.074e6, "USB", "", {}));
    CHECK(n.find(7.074e6, "USB") == nullptr && n.size() == 1);
    CHECK(n.set(1e6, "AM", "", {}) && n.size() == 1);
    // rekey
    CHECK(n.rekey(145.5e6, "NFM", 145.6e6, "NFM"));
    CHECK(n.find(145.5e6, "NFM") == nullptr && n.find(145.6e6, "NFM") != nullptr);
    CHECK(n.rekey(145.6e6, "NFM", 145.6e6, "NFM"));
    CHECK(!n.rekey(1.0e6, "AM", 2.0e6, "AM"));
    CHECK(n.set(100e6, "WFM", "x", {}));
    CHECK(!n.rekey(100e6, "WFM", 145.6e6, "NFM"));   // the target already has a record
    CHECK(n.find(100e6, "WFM") != nullptr);
    CHECK(n.remove(100e6, "WFM") && !n.remove(100e6, "WFM"));
    // refusals change nothing
    const nlohmann::json before = n.toJson();
    CHECK(!n.set(NAN, "AM", "x", {}));
    CHECK(!n.set(-1.0, "AM", "x", {}));
    CHECK(!n.set(1e14, "AM", "x", {}));
    CHECK(!n.set(1e6, "bad mode", "x", {}));
    CHECK(!n.set(1e6, "AM", std::string(2001, 'a'), {}));
    CHECK(n.set(1e6, "AM", std::string(2000, 'a'), {}) );
    CHECK(n.remove(1e6, "AM"));
    CHECK(!n.set(1e6, "AM", "bell\x07", {}));
    CHECK(!n.set(1e6, "AM", "\xC3", {}));             // broken UTF-8
    CHECK(!n.set(1e6, "AM", "x", {"a,b"}));
    CHECK(!n.set(1e6, "AM", "x", {std::string(33, 't')}));
    CHECK(!n.set(1e6, "AM", "x", {"tab\there"}));
    std::vector<std::string> many;
    for (int i = 0; i < 17; ++i) { many.push_back("t" + std::to_string(i)); }
    CHECK(!n.set(1e6, "AM", "x", many));
    many.pop_back();
    CHECK(n.set(1e6, "AM", "x", many));
    CHECK(n.remove(1e6, "AM"));
    CHECK(n.toJson() == before);
    // the entry limit
    ct::BookmarkNotes full;
    for (std::size_t i = 0; i < ct::BookmarkNotes::kMaxEntries; ++i) { CHECK(full.set(1e6 + static_cast<double>(i), "AM", "n", {})); }
    CHECK(!full.set(9e9, "AM", "n", {}, &err) && err == "too many notes");
    CHECK(full.set(1e6, "AM", "changed", {}));          // replacing is still fine
    CHECK(full.size() == ct::BookmarkNotes::kMaxEntries);
}

void testNotesJson() {
    ct::BookmarkNotes a;
    CHECK(a.set(145.5e6, "NFM", "a \xC3\xA9 note\nwith two lines", {"x", "y"}));
    CHECK(a.set(7.074e6, "USB", "", {"only-tag"}));
    ct::BookmarkNotes b;
    std::string err;
    CHECK(b.fromJson(nlohmann::json::parse(a.toJson().dump()), &err));
    CHECK(b.toJson() == a.toJson());
    CHECK(a.toJson()["version"] == 1);
    const nlohmann::json good = a.toJson();
    auto refuse = [&](nlohmann::json bad) {
        ct::BookmarkNotes c;
        CHECK(c.set(1e6, "AM", "keep", {}));
        const bool ok = c.fromJson(bad, &err);
        CHECK(!ok);
        CHECK(c.size() == 1 && c.find(1e6, "AM") != nullptr);   // untouched
    };
    nlohmann::json v = good; v["version"] = 2; refuse(v);
    v = good; v.erase("version"); refuse(v);
    v = good; v["notes"] = 5; refuse(v);
    v = good; v["notes"][0]["hz"] = 1.5; refuse(v);
    v = good; v["notes"][0]["hz"] = -4; refuse(v);
    v = good; v["notes"][0]["mode"] = "a b"; refuse(v);
    v = good; v["notes"][0]["note"] = 7; refuse(v);
    v = good; v["notes"][0]["tags"] = "t"; refuse(v);
    v = good; v["notes"][0]["tags"] = nlohmann::json::array({"UPPER"}); refuse(v);        // a tag must already be normal
    v = good; v["notes"][0]["tags"] = nlohmann::json::array({"a", "a"}); refuse(v);
    v = good; v["notes"].push_back(v["notes"][0]); refuse(v);                              // a repeated key
    v = good; v["notes"][1]["tags"] = nlohmann::json::array(); v["notes"][1]["note"] = ""; refuse(v);   // an empty record
    v = good; v["notes"][0]["note"] = "bad\x01"; refuse(v);
    refuse(nlohmann::json::array());
    refuse(nlohmann::json());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
ct::FavouriteState fav(const char* name) {
    ct::FavouriteState f;
    f.name = name;
    f.freqHz = 145.5e6; f.mode = "NFM"; f.bandwidthHz = 12500; f.displayCentreHz = 145.6e6; f.displaySpanHz = 1.2e6;
    f.spectrumTopDb = -30; f.spectrumRangeDb = 80; f.waterfallFloorDb = -100; f.waterfallCeilingDb = -40;
    f.dsp.agc = "fast"; f.dsp.noiseReduction = true; f.dsp.noiseReductionLevel = 0.7; f.dsp.noiseBlanker = true; f.dsp.notch = true;
    f.dsp.squelchDb = -70; f.dsp.deemphasis = "75us";
    return f;
}

void testFavourites() {
    ct::FavouriteState f = fav("Local repeater");
    CHECK(ct::validFavourite(f));
    // each field out of range is refused
    auto bad = [&](auto edit) { ct::FavouriteState g = fav("x"); edit(g); CHECK(!ct::validFavourite(g)); };
    bad([](auto& g) { g.name = ""; });
    bad([](auto& g) { g.name = std::string(65, 'n'); });
    bad([](auto& g) { g.name = "a\nb"; });
    bad([](auto& g) { g.freqHz = 0; });
    bad([](auto& g) { g.freqHz = NAN; });
    bad([](auto& g) { g.mode = "no good"; });
    bad([](auto& g) { g.bandwidthHz = 10; });
    bad([](auto& g) { g.displaySpanHz = 50; });
    bad([](auto& g) { g.displayCentreHz = -1; });
    bad([](auto& g) { g.spectrumRangeDb = 5; });
    bad([](auto& g) { g.spectrumTopDb = INFINITY; });
    bad([](auto& g) { g.waterfallCeilingDb = g.waterfallFloorDb; });
    bad([](auto& g) { g.waterfallCeilingDb = g.waterfallFloorDb - 1; });
    bad([](auto& g) { g.dsp.agc = ""; });
    bad([](auto& g) { g.dsp.noiseReductionLevel = 1.5; });
    bad([](auto& g) { g.dsp.squelchDb = 1; });
    bad([](auto& g) { g.dsp.deemphasis = "a b"; });
    // boundaries accepted
    ct::FavouriteState e = fav("edge");
    e.dsp.noiseReductionLevel = 0; e.dsp.squelchDb = -120; e.bandwidthHz = 50; e.displaySpanHz = 100; e.spectrumRangeDb = 10; e.name = std::string(64, 'n');
    CHECK(ct::validFavourite(e));
    e.dsp.noiseReductionLevel = 1; e.dsp.squelchDb = 0; e.bandwidthHz = 1e7;
    CHECK(ct::validFavourite(e));
    // store
    ct::FavouriteStore s;
    std::string err;
    CHECK(s.set(f, &err));
    CHECK(s.find("Local repeater") != nullptr && s.find("nope") == nullptr);
    ct::FavouriteState g = fav("Local repeater");
    g.freqHz = 146e6;
    CHECK(s.set(g) && s.size() == 1 && s.find("Local repeater")->freqHz == 146e6);   // replaces by name
    CHECK(s.set(fav("b")) && s.set(fav("a")));
    CHECK(s.names() == (std::vector<std::string>{"Local repeater", "a", "b"}));   // sorted by byte
    CHECK(s.rename("a", "c") && s.find("a") == nullptr && s.find("c")->name == "c");
    CHECK(!s.rename("c", "b") && !s.rename("zz", "q") && !s.rename("c", "") && !s.rename("c", "x\ty"));
    CHECK(s.rename("c", "c"));
    CHECK(s.remove("c") && !s.remove("c"));
    ct::FavouriteState invalid = fav("bad");
    invalid.freqHz = -1;
    const std::size_t sz = s.size();
    CHECK(!s.set(invalid, &err) && s.size() == sz);
    ct::FavouriteStore many;
    for (std::size_t i = 0; i < ct::FavouriteStore::kMaxEntries; ++i) { CHECK(many.set(fav(("f" + std::to_string(i)).c_str()))); }
    CHECK(!many.set(fav("one-more"), &err) && err == "more than 200 favourites");
    CHECK(many.set(fav("f0")));
}

void testFavouriteJson() {
    ct::FavouriteStore a;
    CHECK(a.set(fav("one")));
    CHECK(a.set(fav("two")));
    ct::FavouriteStore b;
    std::string err;
    CHECK(b.fromJson(nlohmann::json::parse(a.toJson().dump()), &err));
    CHECK(b.toJson() == a.toJson());
    CHECK(b.find("one")->dsp.agc == "fast" && b.find("one")->dsp.noiseReduction && b.find("one")->waterfallFloorDb == -100);
    const nlohmann::json good = a.toJson();
    auto refuse = [&](nlohmann::json bad) {
        ct::FavouriteStore c;
        CHECK(c.set(fav("keep")));
        CHECK(!c.fromJson(bad, &err));
        CHECK(c.size() == 1 && c.find("keep") != nullptr);
    };
    nlohmann::json v = good; v["version"] = 2; refuse(v);
    v = good; v.erase("favourites"); refuse(v);
    v = good; v["favourites"][0]["freqHz"] = "145"; refuse(v);
    v = good; v["favourites"][0].erase("dsp"); refuse(v);
    v = good; v["favourites"][0]["dsp"]["notch"] = 1; refuse(v);
    v = good; v["favourites"][0]["dsp"].erase("agc"); refuse(v);
    v = good; v["favourites"][0]["waterfallCeilingDb"] = -200; refuse(v);
    v = good; v["favourites"][1]["name"] = "one"; refuse(v);       // repeated name
    v = good; v["favourites"][0] = 5; refuse(v);
    refuse(nlohmann::json::array());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
void testMarkerExport() {
    std::vector<cc::FreqMarker> list;
    list.push_back({1, 145500000.0, 1790000000, "plain note"});
    list.push_back({3, 7074050.25, 1790000100, "has, comma and \"quotes\"\nand a line"});
    list.push_back({7, 14074000.0, 0, "=HYPERLINK(\"x\")"});
    list.push_back({8, 100e6, 5, ""});
    const std::string csv = ct::markersToCsv(list);
    CHECK(csv.find("number,freq_hz,noted_unix,note\r\n") == 0);
    CHECK(csv.find(",\"'=HYPERLINK") != std::string::npos);             // guarded in the file
    std::vector<cc::FreqMarker> back;
    std::string err;
    CHECK(ct::parseMarkersCsv(csv, back, err));
    CHECK(back.size() == 4);
    if (back.size() == 4) {
        CHECK(back[0].number == 1 && back[0].freqHz == 145500000.0 && back[0].notedUnix == 1790000000 && back[0].note == "plain note");
        CHECK(back[1].note == "has, comma and \"quotes\"\nand a line");
        CHECK_NEAR(back[1].freqHz, 7074050.25, 0.001);
        CHECK(back[2].note == "=HYPERLINK(\"x\")");                    // the guard is removed on reading
        CHECK(back[3].note.empty());
    }
    // without the guard the file holds the note as typed
    CHECK(ct::markersToCsv(list, false).find(",\"=HYPERLINK") != std::string::npos);
    std::vector<cc::FreqMarker> raw;
    CHECK(ct::parseMarkersCsv(ct::markersToCsv(list, false), raw, err, false) && raw[2].note == "=HYPERLINK(\"x\")");
    // the stated cost: a note that really starts with '= loses its apostrophe through the guarded path
    std::vector<cc::FreqMarker> odd = {{1, 1e6, 1, "'=odd"}};
    std::vector<cc::FreqMarker> oddBack;
    CHECK(ct::parseMarkersCsv(ct::markersToCsv(odd), oddBack, err) && oddBack[0].note == "=odd");   // written as '=odd (it starts with ', so no guard is added); the reader removes the apostrophe
    // JSON is exact
    std::vector<cc::FreqMarker> jb;
    CHECK(ct::parseMarkersJson(nlohmann::json::parse(ct::markersToJson(list).dump()), jb, err));
    CHECK(jb.size() == 4 && jb[1].note == list[1].note && jb[1].freqHz == 7074050.25 && jb[2].number == 7);
    // strict refusals, each leaves `out` empty
    auto badCsv = [&](const std::string& t) { std::vector<cc::FreqMarker> o(1); CHECK(!ct::parseMarkersCsv(t, o, err)); CHECK(o.empty()); };
    badCsv("");
    badCsv("n,f\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,2,3\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n0,1e6,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n-1,1e6,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1x,1e6,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,abc,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,nan,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,-5,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e14,1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e6,-1,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e6,1x,x\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e6,1,x\r\n1,2e6,1,y\r\n");   // repeated number
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e6,1,\"unterminated\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e6,1,bell\x07\r\n");
    badCsv("number,freq_hz,noted_unix,note\r\n1,1e6,1," + std::string(501, 'a') + "\r\n");
    std::string rowsOver = "number,freq_hz,noted_unix,note\r\n";
    for (int i = 1; i <= 200; ++i) { rowsOver += std::to_string(i) + ",1e6,1,x\r\n"; }
    CHECK(ct::parseMarkersCsv(rowsOver, back, err) && back.size() == 200);       // the edge: exactly a full list
    badCsv(rowsOver + "201,1e6,1,x\r\n");                                       // one over
    auto badJson = [&](nlohmann::json j) { std::vector<cc::FreqMarker> o(1); CHECK(!ct::parseMarkersJson(j, o, err)); CHECK(o.empty()); };
    const nlohmann::json gj = ct::markersToJson(list);
    nlohmann::json v = gj; v["version"] = 3; badJson(v);
    v = gj; v["markers"][0]["n"] = 0; badJson(v);
    v = gj; v["markers"][0]["freqHz"] = "x"; badJson(v);
    v = gj; v["markers"][1]["n"] = 1; badJson(v);
    v = gj; v["markers"][0]["note"] = std::string(501, 'x'); badJson(v);
    v = gj; v["markers"][0].erase("noted"); badJson(v);
    badJson(nlohmann::json::array());
}

void testMarkerImportInto() {
    cc::FreqMarkers live;
    live.add(145.5e6, 100);
    live.setNote(1, "mine");
    live.add(100e6, 100);                                    // number 2, no note
    std::vector<cc::FreqMarker> in = {{9, 145.5e6 + 100.0, 5, "theirs"}, {10, 100e6 + 50.0, 6, "gets a note"}, {11, 7.074e6, 7, "new one"}, {12, 14.074e6, 8, ""}};
    const ct::MarkerImportResult r = ct::importInto(live, in, 500.0);
    CHECK(r.merged == 2 && r.added == 2 && !r.listFull);
    CHECK(live.size() == 4);
    std::string note1, note2, noteNew;
    int newNumber = 0;
    for (const cc::FreqMarker& m : live.list()) {
        if (m.number == 1) { note1 = m.note; }
        if (m.number == 2) { note2 = m.note; }
        if (m.freqHz == 7.074e6) { noteNew = m.note; newNumber = m.number; }
    }
    CHECK(note1 == "mine");              // an existing note is kept
    CHECK(note2 == "gets a note");       // an empty one is filled
    CHECK(noteNew == "new one" && newNumber == 3);   // a new number, not the file's 11
    // a full list stops the import
    cc::FreqMarkers full;
    for (int i = 0; i < 200; ++i) { full.add(1e6 + i * 1000.0, 1); }
    const ct::MarkerImportResult f = ct::importInto(full, {{1, 500e6, 1, "x"}}, 0.0);
    CHECK(f.added == 0 && f.listFull && full.size() == 200);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
const char* kStations =
    "Frequency,Station,Language,Power kW,Note\r\n"
    "6195,Radio One,English,100,\"day, night\"\r\n"
    "6195,Radio Two,German,50,\r\n"
    "9410,Radio Three,Arabic,250,=cmd\r\n"
    "not a number,Radio Four,French,10,\r\n"
    "-5,Radio Five,French,10,\r\n"
    "7200\r\n"
    "11950,Radio Six,Spanish,100,\xC3\x28 broken\r\n";

void testOverlay() {
    ct::OverlayConfig cfg;
    cfg.freqColumn = "FREQUENCY";                       // header match is case-insensitive
    cfg.unit = ct::FreqUnit::kHz;
    cfg.columns = {"station", "Language", "power kw"};
    ct::StationOverlay o;
    std::string err;
    CHECK(o.load(kStations, cfg, err));
    CHECK(o.size() == 5);                               // 5 good rows (the short row "7200" has a frequency and empty cells); "not a number" and "-5" are skipped
    CHECK(o.skippedRows() == 2);
    CHECK(o.cleanedCells() == 0);                       // the broken cell is in a column that was not kept
    CHECK(o.columnNames() == (std::vector<std::string>{"station", "Language", "power kw"}));
    auto f = o.lookup(6195e3, 500.0);
    CHECK(f.size() == 3 && f[0].second == "Radio One" && f[1].second == "English" && f[2].second == "100");
    auto all = o.lookupAll(6195e3, 500.0);
    CHECK(all.size() == 2 && all[0][0].second == "Radio One" && all[1][0].second == "Radio Two");   // tie: file order (stable)
    CHECK(o.lookup(6196e3, 500.0).empty());
    CHECK(o.lookup(6196e3, 1000.0).size() == 3);        // exactly at the tolerance counts
    CHECK(o.lookupAll(6195e3, 500.0, 1).size() == 1);
    CHECK(o.lookupAll(6195e3, 500.0, 0).empty());
    CHECK(o.lookup(NAN, 500.0).empty() && o.lookup(6195e3, -1.0).empty() && o.lookup(6195e3, NAN).empty());
    CHECK(o.frequencyOf(0) == 6195e3 && o.frequencyOf(99) == 0.0);
    // nearest wins
    ct::StationOverlay n;
    CHECK(n.load("f,s\n100,A\n103,B\n", [] { ct::OverlayConfig c; c.freqColumn = "f"; c.unit = ct::FreqUnit::Hz; c.columns = {"s"}; return c; }(), err));
    CHECK(n.lookup(102.0, 5.0)[0].second == "B" && n.lookup(101.0, 5.0)[0].second == "A");
    CHECK(n.lookup(101.5, 5.0)[0].second == "A");       // an exact tie: the lower frequency
    // units
    ct::OverlayConfig m = cfg;
    m.freqColumn = "Frequency"; m.unit = ct::FreqUnit::MHz; m.columns = {"Note"};
    ct::StationOverlay mm;
    CHECK(mm.load("Frequency,Note\n6.195,x\n", m, err));
    CHECK(mm.frequencyOf(0) > 6.194999e6 && mm.frequencyOf(0) < 6.195001e6);
    // a cell that is not valid text is cleaned and counted, never dropped
    ct::OverlayConfig cl = cfg;
    cl.columns = {"Station", "Note"};
    ct::StationOverlay c2;
    CHECK(c2.load("Frequency,Station,Note\n7000,\"two\nlines\",ok\n7001,bad\x07here,\xC3\x28x\n", cl, err));
    CHECK(c2.size() == 2 && c2.cleanedCells() == 3);
    CHECK(c2.lookup(7000e3, 1.0)[0].second == "two lines");
    CHECK(c2.lookup(7001e3, 1.0)[0].second == "bad here");
    CHECK(c2.lookup(7001e3, 1.0)[1].second == " (x");      // the bad lead byte is a space; the byte after it stays
    // semicolon files
    ct::OverlayConfig sc = cfg;
    sc.delimiter = ';'; sc.freqColumn = "f"; sc.unit = ct::FreqUnit::Hz; sc.columns = {"s"};
    ct::StationOverlay s2;
    CHECK(s2.load("f;s\n1000;\"a;b\"\n", sc, err) && s2.lookup(1000.0, 0.0)[0].second == "a;b");
    // a missing column refuses the whole load and names it
    ct::OverlayConfig miss = cfg;
    miss.columns = {"Station", "Transmitter"};
    ct::StationOverlay x;
    CHECK(!x.load(kStations, miss, err) && err == "no column named Transmitter" && x.size() == 0);
    miss = cfg; miss.freqColumn = "Freq";
    CHECK(!x.load(kStations, miss, err) && err == "no column named Freq");
    CHECK(!x.load("", cfg, err));
    CHECK(!x.load("Frequency,Station\n\"unterminated\n", cfg, err));
    ct::OverlayConfig inv = cfg;
    inv.columns.clear();
    CHECK(!x.load(kStations, inv, err));
    // a failed load leaves the overlay empty even when it held something
    CHECK(o.load(kStations, cfg, err) && o.size() == 5);
    CHECK(!o.load(kStations, miss, err) && o.size() == 0 && o.columnNames().empty());
}

void testOverlayConfig() {
    ct::OverlayConfig c;
    c.freqColumn = "Freq"; c.unit = ct::FreqUnit::MHz; c.columns = {"a", "b"}; c.delimiter = ';';
    ct::OverlayConfig d;
    std::string err;
    CHECK(ct::overlayConfigFromJson(nlohmann::json::parse(ct::overlayConfigToJson(c).dump()), d, &err));
    CHECK(d.freqColumn == "Freq" && d.unit == ct::FreqUnit::MHz && d.columns == c.columns && d.delimiter == ';');
    const nlohmann::json good = ct::overlayConfigToJson(c);
    auto refuse = [&](nlohmann::json bad) { ct::OverlayConfig k = c; CHECK(!ct::overlayConfigFromJson(bad, k, &err)); CHECK(k.freqColumn == "Freq"); };
    nlohmann::json v = good; v["version"] = 2; refuse(v);
    v = good; v["unit"] = "GHz"; refuse(v);
    v = good; v["delimiter"] = "|"; refuse(v);
    v = good; v["delimiter"] = ";;"; refuse(v);
    v = good; v["columns"] = nlohmann::json::array(); refuse(v);
    v = good; v["columns"] = nlohmann::json::array({1}); refuse(v);
    v = good; v["columns"] = nlohmann::json::array(); for (int i = 0; i < 17; ++i) { v["columns"].push_back("c" + std::to_string(i)); } refuse(v);
    v = good; v["freqColumn"] = ""; refuse(v);
    v = good; v.erase("unit"); refuse(v);
    refuse(nlohmann::json::array());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
std::vector<cc::Bookmark> sampleBookmarks() {
    std::vector<cc::Bookmark> v;
    cc::Bookmark a; a.name = "2m repeater"; a.freqHz = 145.6e6; a.mode = "NFM"; a.bandwidthHz = 12500; a.group = "VHF"; a.favourite = true; a.id = 77; a.scan = true; a.heardSeconds = 123.0;
    cc::Bookmark b; b.name = "Airband"; b.freqHz = 121.5e6; b.mode = "AM"; b.bandwidthHz = 8000;
    v.push_back(a); v.push_back(b);
    return v;
}

void testPack() {
    ct::BookmarkNotes notes;
    CHECK(notes.set(145.6e6, "NFM", "Hears the club net on Sundays", {"club", "net"}));
    const std::vector<cc::Bookmark> list = sampleBookmarks();
    int skipped = -1;
    const std::string text = ct::exportPack("My pack", "A starter set", list, &notes, {fav("f1")}, &skipped);
    CHECK(skipped == 0);
    // nothing that belongs to this session or this user
    CHECK(text.find("\"id\"") == std::string::npos && text.find("heard") == std::string::npos && text.find("\"scan\"") == std::string::npos &&
          text.find("author") == std::string::npos && text.find("77") == std::string::npos);
    ct::Pack p;
    std::string err;
    CHECK(ct::importPack(text, p, err));
    CHECK(p.name == "My pack" && p.description == "A starter set" && p.bookmarks.size() == 2 && p.favourites.size() == 1);
    CHECK(p.bookmarks[0].bookmark.name == "2m repeater" && p.bookmarks[0].bookmark.group == "VHF" && p.bookmarks[0].bookmark.favourite);
    CHECK(p.bookmarks[0].bookmark.id == 0 && !p.bookmarks[0].bookmark.scan && p.bookmarks[0].bookmark.heardSeconds == 0.0);
    CHECK(p.bookmarks[0].note.note == "Hears the club net on Sundays" && p.bookmarks[0].note.tags == (std::vector<std::string>{"club", "net"}));
    CHECK(p.bookmarks[1].note.note.empty() && p.bookmarks[1].note.tags.empty());
    CHECK(p.favourites[0].name == "f1" && p.favourites[0].dsp.agc == "fast");
    // a BOM is tolerated
    CHECK(ct::importPack("\xEF\xBB\xBF" + text, p, err));
    // invalid bookmarks are left out at export, and counted
    std::vector<cc::Bookmark> withBad = list;
    withBad[1].freqHz = NAN;
    withBad.push_back(list[0]); withBad.back().name = "";
    ct::exportPack("n", "d", withBad, nullptr, {}, &skipped);
    CHECK(skipped == 2);
    // the import plan: duplicates (same mode within 1 Hz) are left out, never overwritten
    ct::Pack q;
    CHECK(ct::importPack(ct::exportPack("n", "d", list, nullptr, {}), q, err));
    std::vector<cc::Bookmark> mine = {list[0]};
    mine[0].name = "my own name";
    ct::PackImportPlan plan = ct::planImport(q, mine);
    CHECK(plan.duplicates == 1 && plan.toAdd.size() == 1 && plan.toAdd[0].bookmark.name == "Airband");
    mine[0].mode = "AM";                                  // same frequency, another mode: not a duplicate
    CHECK(ct::planImport(q, mine).toAdd.size() == 2);
    mine[0].mode = "NFM"; mine[0].freqHz += 2.0;          // 2 Hz away: not a duplicate at 1 Hz, one at 5 Hz
    CHECK(ct::planImport(q, mine).toAdd.size() == 2);
    CHECK(ct::planImport(q, mine, 5.0).toAdd.size() == 1);
    // a pack that repeats itself
    ct::Pack twice = q;
    twice.bookmarks.push_back(twice.bookmarks[0]);
    CHECK(ct::planImport(twice, {}).toAdd.size() == 2 && ct::planImport(twice, {}).duplicates == 1);
    CHECK(ct::planImport(ct::Pack(), {}).toAdd.empty());
}

void testPackRefusals() {
    const std::vector<cc::Bookmark> list = sampleBookmarks();
    const nlohmann::json good = nlohmann::json::parse(ct::exportPack("n", "d", list, nullptr, {fav("f1")}));
    auto refuse = [&](nlohmann::json bad, const char* what) {
        ct::Pack p;
        p.name = "keep";
        std::string err;
        const bool ok = ct::importPack(bad.dump(), p, err);
        CHECK(!ok);
        CHECK(p.name == "keep");
        CHECK(!err.empty());
        if (ok) { std::printf("  accepted: %s\n", what); }
    };
    nlohmann::json v = good; v["format"] = "other"; refuse(v, "format");
    v = good; v.erase("format"); refuse(v, "no format");
    v = good; v["version"] = 2; refuse(v, "version 2");
    v = good; v["version"] = "1"; refuse(v, "version text");
    v = good; v["bookmarks"][0]["freqHz"] = -1; refuse(v, "negative frequency");
    v = good; v["bookmarks"][0]["freqHz"] = 1e14; refuse(v, "huge frequency");
    v = good; v["bookmarks"][0]["mode"] = "x y"; refuse(v, "mode");
    v = good; v["bookmarks"][0]["bandwidthHz"] = 1; refuse(v, "bandwidth");
    v = good; v["bookmarks"][0]["name"] = ""; refuse(v, "empty name");
    v = good; v["bookmarks"][0]["name"] = std::string(65, 'n'); refuse(v, "long name");
    v = good; v["bookmarks"][0]["group"] = "a\nb"; refuse(v, "group");
    v = good; v["bookmarks"][0]["favourite"] = 1; refuse(v, "favourite type");
    v = good; v["bookmarks"][0]["tags"] = nlohmann::json::array({"a,b"}); refuse(v, "tag");
    v = good; v["bookmarks"][0].erase("note"); refuse(v, "no note");
    v = good; v["bookmarks"][0]["note"] = std::string(2001, 'x'); refuse(v, "long note");
    v = good; v["favourites"][0]["freqHz"] = 0; refuse(v, "favourite");
    v = good; v["name"] = std::string(65, 'x'); refuse(v, "pack name");
    v = good; v["description"] = std::string(501, 'x'); refuse(v, "pack description");
    v = good; v["bookmarks"] = nlohmann::json::array(); for (int i = 0; i < 5001; ++i) { v["bookmarks"].push_back(good["bookmarks"][0]); } refuse(v, "5001 bookmarks");
    refuse(nlohmann::json::array(), "array");
    ct::Pack p;
    std::string err;
    CHECK(!ct::importPack("", p, err));
    CHECK(!ct::importPack("{not json", p, err));
    CHECK(!ct::importPack(std::string(ct::kPackMaxBytes + 1, ' '), p, err) && err.find("4 MiB") != std::string::npos);
    // exactly 5000 bookmarks is accepted
    v = good; v["bookmarks"] = nlohmann::json::array(); for (int i = 0; i < 5000; ++i) { v["bookmarks"].push_back(good["bookmarks"][0]); }
    CHECK(ct::importPack(v.dump(), p, err) && p.bookmarks.size() == 5000);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
std::string mutate(const std::string& base, Lcg& r) {
    std::string s = base;
    const int edits = 1 + static_cast<int>(r.u32() % 4);
    for (int e = 0; e < edits; ++e) {
        switch (r.u32() % 5) {
            case 0: s[r.u32() % s.size()] = static_cast<char>(r.u32() & 0xFF); break;
            case 1: s.erase(r.u32() % s.size(), 1 + r.u32() % 8); break;
            case 2: s.insert(r.u32() % s.size(), 1, "{}[]\":,0123456789e-+.\r\n;'="[r.u32() % 28]); break;
            case 3: s.resize(1 + r.u32() % s.size()); break;
            default: { const std::size_t at = r.u32() % s.size(); s.insert(at, s.substr(r.u32() % s.size(), 1 + r.u32() % 6)); break; }
        }
        if (s.empty()) { s = "x"; }
    }
    return s;
}

void fuzzAll() {
    Lcg r(2026);
    const int kRuns = 20000;
    int csvOk = 0, markerOk = 0, markerJsonOk = 0, overlayOk = 0, packOk = 0, notesOk = 0, favOk = 0;
    // seeds
    std::vector<cc::FreqMarker> list = {{1, 145.5e6, 100, "a,b"}, {2, 7.074e6, 200, "=x\n\"y\""}};
    const std::string markerCsv = ct::markersToCsv(list);
    const std::string markerJson = ct::markersToJson(list).dump();
    ct::OverlayConfig oc;
    oc.freqColumn = "Frequency"; oc.columns = {"Station", "Language"};
    ct::BookmarkNotes notes;
    notes.set(145.6e6, "NFM", "n", {"t"});
    const std::string notesJson = notes.toJson().dump();
    ct::FavouriteStore fs;
    fs.set(fav("f"));
    const std::string favJson = fs.toJson().dump();
    const std::string pack = ct::exportPack("n", "d", sampleBookmarks(), &notes, {fav("f")});
    for (int i = 0; i < kRuns; ++i) {
        Rows rows; std::string err;
        if (ct::parseCsv(mutate(markerCsv, r), rows, ct::CsvLimits(), err)) { ++csvOk; }
        std::vector<cc::FreqMarker> out;
        if (ct::parseMarkersCsv(mutate(markerCsv, r), out, err)) {
            ++markerOk;
            for (const auto& m : out) { CHECK(ct::validMarker(m)); }
        }
        const nlohmann::json mj = nlohmann::json::parse(mutate(markerJson, r), nullptr, false);
        if (!mj.is_discarded() && ct::parseMarkersJson(mj, out, err)) { ++markerJsonOk; for (const auto& m : out) { CHECK(ct::validMarker(m)); } }
        ct::StationOverlay so;
        if (so.load(mutate(kStations, r), oc, err)) {
            ++overlayOk;
            so.lookup(6195e3, 1000.0);
        }
        ct::Pack p;
        if (ct::importPack(mutate(pack, r), p, err)) {
            ++packOk;
            for (const auto& b : p.bookmarks) { CHECK(ct::validPackBookmark(b)); }
            for (const auto& f : p.favourites) { CHECK(ct::validFavourite(f)); }
        }
        const nlohmann::json nj = nlohmann::json::parse(mutate(notesJson, r), nullptr, false);
        ct::BookmarkNotes nn;
        if (!nj.is_discarded() && nn.fromJson(nj)) { ++notesOk; }
        const nlohmann::json fj = nlohmann::json::parse(mutate(favJson, r), nullptr, false);
        ct::FavouriteStore ff;
        if (!fj.is_discarded() && ff.fromJson(fj)) { ++favOk; }
    }
    std::printf("fuzz: %d runs each; accepted csv %d, marker csv %d, marker json %d, overlay %d, pack %d, notes %d, favourites %d\n", kRuns, csvOk, markerOk, markerJsonOk, overlayOk,
                packOk, notesOk, favOk);
    CHECK(csvOk > 100 && overlayOk > 100);
}

}  // namespace

int main() {
    testCsvRead();
    testCsvWrite();
    testNotes();
    testNotesJson();
    testFavourites();
    testFavouriteJson();
    testMarkerExport();
    testMarkerImportInto();
    testOverlay();
    testOverlayConfig();
    testPack();
    testPackRefusals();
    fuzzAll();
    return testSummary("test_tuning_bookmarks");
}
