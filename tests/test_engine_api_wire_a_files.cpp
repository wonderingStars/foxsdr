// test_engine_api_wire_a_files.cpp - THE FILES OF THE USER'S CHOOSING (wiring lane wire-a), held against the real engine through its API: markers, packs, notes ...
//
//   F1  MARKER EXPORT / IMPORT: markers dropped through MARKER_DROP are exported as CSV and JSON, the list is cleared, the file imports back to the same list (numbers new, notes
//       kept); a second import merges; a file that exists is never overwritten; a relative path, a bad direction, a missing file, a file with one bad row (nothing changes),
//       a formula note guarded in the CSV and unguarded on the way back.
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

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_files");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_files_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testMarkers();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_files");
}
