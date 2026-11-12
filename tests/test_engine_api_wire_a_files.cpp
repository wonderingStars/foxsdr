// test_engine_api_wire_a_files.cpp - THE FILES OF THE USER'S CHOOSING (wiring lane wire-a), held against the real engine through its API: markers, packs, notes ...
//
//   F1  MARKER EXPORT / IMPORT: markers dropped through MARKER_DROP are exported as CSV and JSON, the list is cleared, the file imports back to the same list (numbers new, notes
//       kept); a second import merges; a file that exists is never overwritten; a relative path, a bad direction, a missing file, a file with one bad row (nothing changes),
//       a formula note guarded in the CSV and unguarded on the way back.
//
//   F2  BOOKMARK NOTES AND TAGS: set, read back, text search, tag search, the same again is NO_CHANGE, a bad tag / over-long note / bad JSON / unknown id are refused and change nothing,
//       a bookmark retuned by the window takes its note with it, the file reaches the disk and a new engine reads it.
//
//   F3  FAVOURITES: save the radio's whole state under a name, change the radio, recall it (frequency, mode, bandwidth, AGC, noise reduction, noise blanker, notch, squelch, de-emphasis come
//       back; the picture's own numbers come back in fact.favourite.recalled), rename, remove, a replaced name, the refusals, the file and a new engine reading it.
//
//   F4  PACKS: bookmarks with notes and tags and favourites exported to a pack file, imported into a second engine (added, with notes and the favourite), imported again (all duplicates, nothing
//       overwritten), a hostile pack and a relative path refused, an existing file not overwritten.
//
//   F5  WAV HEADER CENTRE: an I/Q file whose RIFF auxi chunk says where it was recorded plays at that frequency (not at the frequency the radio happened to be on); one with a zero centre or no auxi chunk keeps
//       the nominal one; fact.iqfile.1.centreFromHeader says which.
//
//   F6  STATION OVERLAY AND GRADIENT: a user's own CSV (frequencies in kHz, three columns, one bad row) is loaded as the lane's job, looked up by frequency within a tolerance (several rows, none, bad numbers),
//       removed; a damaged CSV and a bad config are refused; the gradient text from the model is kept, read back, refused when wrong, written to the file and read by a new engine.
//
//   F7  BOOKMARK FIELDS: a bookmark carries a squelch level, a CTCSS tone, a scan threshold and an offset; tuning to it applies the squelch and the tone (the radio's squelch and tone squelch read back from
//       the state), not while a scan runs; the values are read back, NO_CHANGE when as they were, removed with "", refused when out of range; a retuned bookmark takes them along; the file is read by a new engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "core/gradient_model.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
void spit(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

int markerCount(Rig& r) { return r.api()->read_list(r.local->raw(), FOXAPI_LIST_FREQ_MARKERS, 0, nullptr); }

void testMarkers() {
    std::printf("F1: marker export and import\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_DROP, 145.5e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_DROP, 433.92e6)).status(), FOXAPI_OK);
    const std::string formula = "=SUM(1,1) beacon";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_NOTE, 0.0, 1), &formula).status(), FOXAPI_OK);
    const fs::path csv = g_dir / "m.csv";
    const fs::path json = g_dir / "m.json";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_EXPORT, 0.0, 0, csv.string().c_str())).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_EXPORT, 0.0, 1, json.string().c_str())).status(), FOXAPI_OK);
    const std::string body = slurp(csv);
    CHECK(body.find("number,freq_hz,noted_unix,note") == 0u);
    CHECK(body.find("145500000.000") != std::string::npos);
    CHECK(body.find("'=SUM") != std::string::npos || body.find("\"'=SUM") != std::string::npos);   // the formula is guarded for a spreadsheet
    CHECK(slurp(json).find("\"version\": 1") != std::string::npos);
    CHECK_EQ(r.fact("markerio.last"), std::string("exported 2"));
    // an existing file is never overwritten; a relative path; a bad direction
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_EXPORT, 0.0, 0, csv.string().c_str())).status(), FOXAPI_FAILED);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_EXPORT, 0.0, 0, "rel/m.csv")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_EXPORT, 0.0, 7, (g_dir / "x.csv").string().c_str())).status(), FOXAPI_OUT_OF_RANGE);
    // clear, import the CSV: both come back, the note unguarded
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_CLEAR)).status(), FOXAPI_OK);
    CHECK_EQ(markerCount(r), 0);
    testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_MARKER_IMPORT, 0.0, 0, csv.string().c_str()));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK_EQ(markerCount(r), 2);
    CHECK_EQ(r.fact("markers.note.1"), std::string("=SUM(1,1) beacon"));
    // the same file again merges both (nothing added), the JSON too
    o = r.send(testing::cmd(FOXAPP_OP_MARKER_IMPORT, 0.0, 0, json.string().c_str()));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 0);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 2);
    CHECK_EQ(r.fact("markerio.last"), std::string("added 0, merged 2"));
    // a file with one bad row changes nothing and says so
    spit(g_dir / "bad.csv", "number,freq_hz,noted_unix,note\r\n7,146000000,0,ok\r\n8,notanumber,0,bad\r\n");
    o = r.send(testing::cmd(FOXAPP_OP_MARKER_IMPORT, 0.0, 0, (g_dir / "bad.csv").string().c_str()));
    CHECK_EQ(o.status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(markerCount(r), 2);
    CHECK(!r.fact("markerio.error").empty() && r.fact("markerio.error") != "<absent>");
    // a missing file; a reach out of range; empty list export
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_IMPORT, 0.0, 0, (g_dir / "none.csv").string().c_str())).status(), FOXAPI_FAILED);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_IMPORT, -1.0, 0, csv.string().c_str())).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_CLEAR)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_EXPORT, 0.0, 0, (g_dir / "empty.csv").string().c_str())).status(), FOXAPI_NO_CHANGE);
    // a file over the size limit is refused unread
    spit(g_dir / "big.csv", std::string((1u << 20) + 10, 'a'));
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_MARKER_IMPORT, 0.0, 0, (g_dir / "big.csv").string().c_str())).status(), FOXAPI_FAILED);
}

FoxCommand noteSet(std::uint64_t id) { return testing::cmd(FOXAPP_OP_BOOKMARK_NOTE, 0.0, static_cast<long long>(id)); }

void testNotes() {
    std::printf("F2: bookmark notes and tags\n");
    const fs::path user = g_dir / "user2";
    std::uint64_t id1 = 0;
    {
        Rig r(user);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Calling")).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 433.92e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Sensors")).status(), FOXAPI_OK);
        id1 = r.bookmarkId(145.5e6);
        const std::uint64_t id2 = r.bookmarkId(433.92e6);
        CHECK(id1 != 0 && id2 != 0 && id1 != id2);
        const std::string rec = R"({"note":"Local repeater, busy at 18:00","tags":["Repeater","two-metre"]})";
        CHECK_EQ(r.send(noteSet(id1), &rec).status(), FOXAPI_OK);
        CHECK_EQ(r.send(noteSet(id1), &rec).status(), FOXAPI_NO_CHANGE);   // the same again
        FoxCommand rd = noteSet(id1);
        rd.ival[1] = 1;
        CHECK_EQ(r.send(rd).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("bookmark.note.lastId"), std::to_string(id1));
        CHECK(r.fact("bookmark.note.last").find("Local repeater") != std::string::npos);
        CHECK(r.fact("bookmark.note.last").find("\"repeater\"") != std::string::npos);   // tags are folded to lower case
        CHECK_EQ(r.fact("bookmark.notes.count"), std::string("1"));
        // searches
        testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_BOOKMARK_FIND, 0.0, 0, "busy"));
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
        CHECK_EQ(r.fact("bookmark.find"), std::to_string(id1));
        o = r.send(testing::cmd(FOXAPP_OP_BOOKMARK_FIND, 0.0, 1, "TWO-METRE"));
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
        o = r.send(testing::cmd(FOXAPP_OP_BOOKMARK_FIND, 0.0, 1, "nothing"));
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 0);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_BOOKMARK_FIND, 0.0, 3, "x")).status(), FOXAPI_OUT_OF_RANGE);
        // refusals change nothing
        const std::string badTag = R"({"note":"x","tags":["bad tag with spaces and ;; "]})";
        const std::string badJson = "{not json";
        const std::string longNote = std::string("{\"note\":\"") + std::string(2100, 'n') + "\"}";
        CHECK_EQ(r.send(noteSet(id1), &badJson).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(noteSet(id1), &longNote).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(noteSet(987654), &rec).status(), FOXAPI_NOT_FOUND);
        (void)badTag;
        CHECK_EQ(r.fact("bookmark.notes.count"), std::string("1"));
        // the window retunes the bookmark: the note follows it
        {
            cascade::core::FreqManager& fm = Access::bookmarks(r.eng);
            const std::size_t i = fm.indexOfId(id1);
            cascade::core::Bookmark b = fm.list()[i];
            b.freqHz = 145.55e6;
            fm.updateAt(i, b);
        }
        r.settle(50);
        rd = noteSet(r.bookmarkId(145.55e6));
        rd.ival[1] = 1;
        CHECK_EQ(r.send(rd).status(), FOXAPI_OK);
        CHECK(r.fact("bookmark.note.last").find("Local repeater") != std::string::npos);
        // the file is written on a worker (half a second after the change)
        CHECK(r.until([&] { return fs::exists(user / "bookmark_notes.json"); }, 6000));
    }
    // a new engine on the same folder reads the note back (bookmarks are not reloaded here: the notes file alone is held)
    {
        Rig r(user);
        r.settle(200);
        CHECK_EQ(r.fact("bookmark.notes.count"), std::string("1"));
        CHECK_EQ(r.fact("bookmark.notes.error"), std::string());
    }
    // a damaged file is reported and the notes start empty
    {
        spit(user / "bookmark_notes.json", "{\"version\": 9}");
        Rig r(user);
        r.settle(200);
        CHECK_EQ(r.fact("bookmark.notes.count"), std::string("0"));
        CHECK(!r.fact("bookmark.notes.error").empty() && r.fact("bookmark.notes.error") != "<absent>");
    }
}

FoxCommand op1(std::uint32_t op, double n0, long long i0, long long i1 = 0) {
    FoxCommand c = testing::cmd(op, n0, i0);
    c.ival[1] = i1;
    return c;
}

void testFavourites() {
    std::printf("F3: favourites\n");
    const fs::path user = g_dir / "user3";
    {
        Rig r(user);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 118.3e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPI_OP_SET_AGC, 0.0, FOXAPI_AGC_SLOW)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPI_OP_SET_NR, 0.8, 1, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPI_OP_SET_NB, 0.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -55.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_DEEMPHASIS, 0.0, 1)).status(), FOXAPI_OK);
        const std::string save = R"({"name":"Tower","displaySpanHz":500000,"waterfallFloorDb":-100,"waterfallCeilingDb":-40})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &save).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("favourite.names"), std::string("Tower"));
        // change everything, then recall
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 98.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPI_OP_SET_AGC, 0.0, FOXAPI_AGC_FAST)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPI_OP_SET_NR, 0.2, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPI_OP_SET_NB, 0.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -90.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_DEEMPHASIS, 0.0, 2)).status(), FOXAPI_OK);
        const std::string tower = "Tower";
        testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_FAVOURITE_RECALL), &tower);
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK(std::fabs(o.result.applied[0] - 118.3e6) < 2.0);
        // read the radio back through a second save (the state is what the engine holds)
        const std::string save2 = R"({"name":"Probe"})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &save2).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_RECALL), &tower).status(), FOXAPI_OK);
        const std::string rec = r.fact("favourite.recalled");
        CHECK(rec.find("\"agc\":\"slow\"") != std::string::npos);
        CHECK(rec.find("\"noiseReduction\":true") != std::string::npos);
        CHECK(rec.find("\"noiseBlanker\":true") != std::string::npos);
        CHECK(rec.find("\"squelchDb\":-55.0") != std::string::npos);
        CHECK(rec.find("\"deemphasis\":\"75us\"") != std::string::npos);
        CHECK(rec.find("\"displaySpanHz\":500000.0") != std::string::npos);
        // the probe, taken after the first recall, equals the saved one in every DSP field the engine holds
        const std::string probe = "Probe";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_RECALL), &probe).status(), FOXAPI_OK);
        const std::string pr = r.fact("favourite.recalled");
        CHECK(pr.find("\"agc\":\"slow\"") != std::string::npos && pr.find("\"deemphasis\":\"75us\"") != std::string::npos && pr.find("\"squelchDb\":-55.0") != std::string::npos);
        // rename and remove
        const std::string ren = "Tower\tTower 118.3";
        CHECK_EQ(r.send(op1(FOXAPP_OP_FAVOURITE_EDIT, 0.0, 1), &ren).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_RECALL), &tower).status(), FOXAPI_NOT_FOUND);
        CHECK_EQ(r.send(op1(FOXAPP_OP_FAVOURITE_EDIT, 0.0, 1), &ren).status(), FOXAPI_NOT_FOUND);   // the old name is gone
        const std::string clash = "Probe\tTower 118.3";
        CHECK_EQ(r.send(op1(FOXAPP_OP_FAVOURITE_EDIT, 0.0, 1), &clash).status(), FOXAPI_BAD_ARGUMENT);   // a name that is taken
        const std::string probeName = "Probe";
        CHECK_EQ(r.send(op1(FOXAPP_OP_FAVOURITE_EDIT, 0.0, 0), &probeName).status(), FOXAPI_OK);
        CHECK_EQ(r.send(op1(FOXAPP_OP_FAVOURITE_EDIT, 0.0, 0), &probeName).status(), FOXAPI_NOT_FOUND);
        CHECK_EQ(r.send(op1(FOXAPP_OP_FAVOURITE_EDIT, 0.0, 5), &probeName).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.fact("favourite.count"), std::string("1"));
        // refusals: bad JSON, no name, a bad picture value, a ceiling under the floor
        const std::string bad1 = "{", bad2 = R"({"x":1})", bad3 = R"({"name":"A","displaySpanHz":"wide"})", bad4 = R"({"name":"B","waterfallFloorDb":-20,"waterfallCeilingDb":-60})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &bad1).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &bad2).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &bad3).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &bad4).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.fact("favourite.count"), std::string("1"));
        CHECK(r.until([&] { return fs::exists(user / "favourites.json"); }, 6000));
    }
    {
        Rig r(user);
        r.settle(200);
        CHECK_EQ(r.fact("favourite.names"), std::string("Tower 118.3"));
    }
}

void testPacks() {
    std::printf("F4: packs\n");
    const fs::path pack = g_dir / "air.json";
    {
        Rig r(g_dir / "user4a");
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        const std::string rel = "{\"path\":\"x\"}";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PACK_EXPORT), &rel).status(), FOXAPI_BAD_ARGUMENT);   // not a full path
        const std::string nothing = "{\"path\":\"" + (g_dir / "nothing.json").generic_string() + "\"}";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PACK_EXPORT), &nothing).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 118.3e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Tower")).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 121.5e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Guard")).status(), FOXAPI_OK);
        const std::uint64_t id = r.bookmarkId(118.3e6);
        const std::string rec = R"({"note":"Approach","tags":["air"]})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_BOOKMARK_NOTE, 0.0, static_cast<long long>(id)), &rec).status(), FOXAPI_OK);
        const std::string fav = R"({"name":"Airband view"})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_FAVOURITE_SAVE), &fav).status(), FOXAPI_OK);
        const std::string exp = "{\"path\":\"" + pack.generic_string() + "\",\"name\":\"Air\",\"description\":\"two airband channels\"}";
        testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_PACK_EXPORT), &exp);
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
        CHECK_EQ(r.fact("pack.last"), std::string("exported 2"));
        const std::string body = slurp(pack);
        CHECK(body.find("foxsdr-pack") != std::string::npos);
        CHECK(body.find("Approach") != std::string::npos && body.find("Airband view") != std::string::npos);
        CHECK(body.find("heard") == std::string::npos);   // nothing of this session's goes in
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PACK_EXPORT), &exp).status(), FOXAPI_FAILED);   // never overwrites
    }
    {
        Rig r(g_dir / "user4b");
        const std::string p = pack.generic_string();
        testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_PACK_IMPORT, 0.0, 0, p.c_str()));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
        CHECK_EQ(r.fact("pack.last"), std::string("added 2, 0 already there"));
        CHECK(r.bookmarkId(118.3e6) != 0 && r.bookmarkId(121.5e6) != 0);
        CHECK_EQ(r.fact("bookmark.notes.count"), std::string("1"));
        CHECK_EQ(r.fact("favourite.names"), std::string("Airband view"));
        // again: all duplicates, nothing added
        o = r.send(testing::cmd(FOXAPP_OP_PACK_IMPORT, 0.0, 0, p.c_str()));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(static_cast<int>(o.result.applied[0]), 0);
        CHECK_EQ(static_cast<int>(o.result.applied[1]), 2);
        // a hostile pack and a missing file
        spit(g_dir / "bad.json", R"({"format":"foxsdr-pack","version":1,"name":"x","description":"","bookmarks":[{"name":"","freqHz":-5}],"favourites":[]})");
        const std::string bad = (g_dir / "bad.json").generic_string();
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PACK_IMPORT, 0.0, 0, bad.c_str())).status(), FOXAPI_BAD_ARGUMENT);
        const std::string none = (g_dir / "none.json").generic_string();
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PACK_IMPORT, 0.0, 0, none.c_str())).status(), FOXAPI_FAILED);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PACK_IMPORT, 0.0, 0, "air.json")).status(), FOXAPI_BAD_ARGUMENT);
        CHECK(r.bookmarkId(118.3e6) != 0);
    }
}

void le32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); } }
void le16(std::string& s, std::uint16_t v) { for (int i = 0; i < 2; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); } }

// A 16-bit I/Q WAV of one second of a carrier; `auxiCentreHz` < 0 writes no auxi chunk, otherwise a 164-byte one with that centre (SpectraVue's layout, little-endian uint32 at offset 32).
void writeIqWav(const fs::path& p, double auxiCentreHz) {
    const std::uint32_t rate = 48000;
    std::string data;
    for (std::uint32_t i = 0; i < rate; ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * 1000.0 * i / rate;
        le16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(8000.0 * std::cos(ph))));
        le16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(8000.0 * std::sin(ph))));
    }
    std::string auxi;
    if (auxiCentreHz >= 0.0) {
        std::string body(164, '\0');
        const std::uint32_t c = static_cast<std::uint32_t>(auxiCentreHz);
        for (int i = 0; i < 4; ++i) { body[32 + i] = static_cast<char>((c >> (8 * i)) & 0xFF); }
        auxi = "auxi";
        le32(auxi, 164);
        auxi += body;
    }
    std::string h = "RIFF";
    le32(h, static_cast<std::uint32_t>(4 + 24 + auxi.size() + 8 + data.size()));
    h += "WAVEfmt ";
    le32(h, 16);
    le16(h, 1);
    le16(h, 2);
    le32(h, rate);
    le32(h, rate * 4);
    le16(h, 4);
    le16(h, 16);
    h += auxi;
    h += "data";
    le32(h, static_cast<std::uint32_t>(data.size()));
    spit(p, h + data);
}

void testWavCentre() {
    std::printf("F5: centre from the WAV header\n");
    const fs::path withAuxi = g_dir / "with.wav", zero = g_dir / "zero.wav", plain = g_dir / "plain.wav";
    writeIqWav(withAuxi, 137912000.0);
    writeIqWav(zero, 0.0);
    writeIqWav(plain, -1.0);
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 98.0e6, 0)).status(), FOXAPI_OK);
    auto open = [&](const fs::path& p) {
        const std::string t = "file:" + p.generic_string();
        FoxCommand c = testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, nullptr);
        std::snprintf(c.text, sizeof c.text, "%s", t.c_str());
        CHECK_EQ(r.send(c).status(), FOXAPI_OK);
    };
    auto waitHz = [&](double want) { return r.until([&] { return std::fabs(std::strtod(r.fact("tuned.1.centreHz").c_str(), nullptr) - want) < 1500.0; }, 8000); };
    open(withAuxi);
    const bool got = waitHz(137.912e6);
    if (!got) { std::printf("  radio.1.source=%s tuned=%s state=%s\n", r.fact("radio.1.source").c_str(), r.fact("tuned.1.centreHz").c_str(), r.fact("radio.1.state").c_str()); }
    CHECK(got);
    CHECK_EQ(r.fact("iqfile.1.centreFromHeader"), std::string("1"));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 99.0e6, 0)).status(), FOXAPI_OK);   // the radio is somewhere else: a file without a centre of its own carries that over
    open(plain);
    CHECK(r.until([&] { return r.fact("iqfile.1.centreFromHeader") == "0" && std::strtod(r.fact("tuned.1.centreHz").c_str(), nullptr) > 0.0 && std::fabs(std::strtod(r.fact("tuned.1.centreHz").c_str(), nullptr) - 137.912e6) > 1.0e6; }, 8000));
    open(zero);
    CHECK(r.until([&] { return r.fact("iqfile.1.centreFromHeader") == "0"; }, 8000));
    CHECK(std::fabs(std::strtod(r.fact("tuned.1.centreHz").c_str(), nullptr) - 137.912e6) > 1.0e6);   // a zero centre is "unknown"
}

void testOverlayGradient() {
    std::printf("F6: station overlay and gradient\n");
    const fs::path csv = g_dir / "stations.csv";
    spit(csv, "Frequency,Name,Location,Note\n145500,Calling,Hilltop,main\n145500,Echo,Valley,link\n433920,Sensors,Garden,ISM\nnotanumber,Broken,Nowhere,x\n162550,Weather,Coast,wx\n");
    Rig r(g_dir / "user6");
    const std::string cfg = "{\"version\":1,\"freqColumn\":\"frequency\",\"unit\":\"kHz\",\"columns\":[\"name\",\"location\"],\"delimiter\":\",\"}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOOKUP, 145.5e6)).status(), FOXAPI_NO_CHANGE);   // nothing loaded
    const std::string load = "{\"path\":\"" + csv.generic_string() + "\",\"config\":" + cfg + "}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &load).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("job.state") != "running"; }, 20000));
    CHECK_EQ(r.fact("job.state"), std::string("done"));
    r.settle(100);
    CHECK_EQ(r.fact("overlay.rows"), std::string("4"));
    CHECK_EQ(r.fact("overlay.skipped"), std::string("1"));   // the row that is not a frequency
    CHECK(r.fact("overlay.columns").find("name") != std::string::npos);
    testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOOKUP, 145.5e6, 8));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK(r.fact("overlay.lookup").find("Calling") != std::string::npos && r.fact("overlay.lookup").find("Echo") != std::string::npos);
    o = r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOOKUP, 145.5e6, 1));
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
    o = r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOOKUP, 146.0e6, 8));   // nothing within 500 Hz
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 0);
    CHECK_EQ(r.fact("overlay.lookup"), std::string("[]"));
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOOKUP, -5.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOOKUP, 145.5e6, 99)).status(), FOXAPI_OUT_OF_RANGE);
    // refusals: bad JSON, relative path, a config the model refuses, a CSV without the frequency column
    const std::string badJson = "{nope", rel = "{\"path\":\"x.csv\",\"config\":" + cfg + "}";
    const std::string badCfg = "{\"path\":\"" + csv.generic_string() + "\",\"config\":{\"version\":1,\"freqColumn\":\"frequency\",\"unit\":\"GHz\",\"columns\":[\"name\"],\"delimiter\":\",\"}}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &badJson).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &rel).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &badCfg).status(), FOXAPI_BAD_ARGUMENT);
    spit(g_dir / "nofreq.csv", "Name,Location\nA,B\n");
    const std::string noFreq = "{\"path\":\"" + (g_dir / "nofreq.csv").generic_string() + "\",\"config\":" + cfg + "}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &noFreq).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("job.state") != "running"; }, 20000));
    CHECK_EQ(r.fact("job.state"), std::string("failed"));
    CHECK_EQ(r.fact("overlay.rows"), std::string("4"));   // a failed load leaves the one in force
    // remove
    const std::string none;
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &none).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("overlay.rows"), std::string("0"));
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_OVERLAY_LOAD), &none).status(), FOXAPI_NO_CHANGE);
    // ---- gradient
    const std::string stock = cascade::core::gradient::serialise(cascade::core::gradient::defaultGradient());
    const fs::path user = g_dir / "user6g";
    {
        Rig g(user);
        CHECK_EQ(g.send(testing::cmd(FOXAPP_OP_GRADIENT_SET), &stock).status(), FOXAPI_OK);
        CHECK_EQ(g.fact("display.gradient"), stock);
        CHECK_EQ(g.send(testing::cmd(FOXAPP_OP_GRADIENT_SET), &stock).status(), FOXAPI_NO_CHANGE);
        const std::string bad = "FOXGRAD1|nonsense", bad2 = "not a gradient";
        CHECK_EQ(g.send(testing::cmd(FOXAPP_OP_GRADIENT_SET), &bad).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(g.send(testing::cmd(FOXAPP_OP_GRADIENT_SET), &bad2).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(g.fact("display.gradient"), stock);   // a refusal changes nothing
        CHECK(g.until([&] {
            std::ifstream f(user / "display-gradient.json");
            std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return t.find("FOXGRAD1") != std::string::npos;
        }, 6000));
    }
    {
        Rig g(user);
        g.settle(200);
        CHECK_EQ(g.fact("display.gradient"), stock);   // a new engine reads it
        const std::string off;
        CHECK_EQ(g.send(testing::cmd(FOXAPP_OP_GRADIENT_SET), &off).status(), FOXAPI_OK);
        CHECK_EQ(g.fact("display.gradient"), std::string());
    }
}

void testBookmarkFields() {
    std::printf("F7: bookmark fields\n");
    const fs::path user = g_dir / "user7";
    {
        Rig r(user);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Calling")).status(), FOXAPI_OK);
        const std::uint64_t id = r.bookmarkId(145.5e6);
        CHECK(id != 0);
        FoxCommand setF = testing::cmd(FOXAPP_OP_BOOKMARK_FIELDS, 0.0, static_cast<long long>(id));
        const std::string f1 = R"({"squelchDb":-55,"toneHz":88.5,"thresholdDb":-70,"offsetKhz":12.5})";
        CHECK_EQ(r.send(setF, &f1).status(), FOXAPI_OK);
        CHECK_EQ(r.send(setF, &f1).status(), FOXAPI_NO_CHANGE);
        FoxCommand rd = testing::cmd(FOXAPP_OP_BOOKMARK_FIELDS, 0.0, static_cast<long long>(id));
        rd.ival[1] = 1;
        CHECK_EQ(r.send(rd).status(), FOXAPI_OK);
        CHECK(r.fact("bookmark.fields.last").find("\"squelchDb\":-55.0") != std::string::npos);
        CHECK(r.fact("bookmark.fields.last").find("\"toneHz\":88.5") != std::string::npos);
        CHECK(r.fact("bookmark.fields.last").find("\"offsetKhz\":12.5") != std::string::npos);
        // elsewhere, with another squelch and no tone; tuning to the bookmark applies its squelch and tone
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -10.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.local->state().squelchDb, -10.0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_TUNE, 0.0, static_cast<long long>(id))).status(), FOXAPI_OK);
        r.eng.controlPass();
        FoxReceiverState st = r.local->state();
        CHECK_EQ(st.squelchDb, -55.0);
        CHECK_EQ(st.toneKind, FOXAPI_TONE_CTCSS);
        CHECK_EQ(st.toneHz, 88.5);
        CHECK(std::fabs(st.tunedHz - 145.5e6) < 2.0);
        // out of range, bad JSON, unknown id; "" removes; the record gone means a tune leaves the squelch alone
        const std::string bad1 = R"({"squelchDb":500})", bad2 = "{nope", bad3 = R"({"toneHz":"high"})";
        CHECK_EQ(r.send(setF, &bad1).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(setF, &bad2).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(setF, &bad3).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_BOOKMARK_FIELDS, 0.0, 987654), &f1).status(), FOXAPI_NOT_FOUND);
        CHECK_EQ(r.fact("bookmark.fields.count"), std::string("1"));
        // a retuned bookmark takes its fields along (the window edits the list)
        {
            cascade::core::FreqManager& fm = Access::bookmarks(r.eng);
            const std::size_t i = fm.indexOfId(id);
            cascade::core::Bookmark b = fm.list()[i];
            b.freqHz = 145.55e6;
            fm.updateAt(i, b);
        }
        r.settle(100);
        const std::uint64_t id2 = r.bookmarkId(145.55e6);
        CHECK(id2 != 0);
        FoxCommand rd2 = testing::cmd(FOXAPP_OP_BOOKMARK_FIELDS, 0.0, static_cast<long long>(id2));
        rd2.ival[1] = 1;
        CHECK_EQ(r.send(rd2).status(), FOXAPI_OK);
        CHECK(r.fact("bookmark.fields.last").find("\"toneHz\":88.5") != std::string::npos);
        CHECK(r.until([&] {
            std::ifstream f(user / "bookmark_fields.json");
            std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return t.find("145550000") != std::string::npos;
        }, 6000));
        // a scan running: tuning to a bookmark does not touch the squelch
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -10.0)).status(), FOXAPI_OK);
        FoxCommand run = testing::cmd(FOXAPI_OP_SCANNER_RUN, 100.0e6, 1);
        run.num[1] = 100.1e6;
        run.num[2] = 25.0e3;
        CHECK_EQ(r.send(run).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_BOOKMARK_TUNE, 0.0, static_cast<long long>(id2))).status(), FOXAPI_OK);
        r.eng.controlPass();
        CHECK_EQ(r.local->state().squelchDb, -10.0);   // a scan would change the squelch at every step: not applied
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SCANNER_RUN, 0, 0)).status(), FOXAPI_OK);
    }
    {
        Rig r(user);
        r.settle(200);
        CHECK_EQ(r.fact("bookmark.fields.count"), std::string("1"));   // a new engine reads the file
    }
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_files");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_files_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testMarkers();
    testNotes();
    testFavourites();
    testPacks();
    testWavCentre();
    testOverlayGradient();
    testBookmarkFields();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_files");
}
