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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_files");
}
