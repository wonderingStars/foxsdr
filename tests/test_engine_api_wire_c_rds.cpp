// test_engine_api_wire_c_rds.cpp - THE RDS LOG IN THE REAL ENGINE (engine lane E1, FEATURE-GAPS G4-53; core/rds_log.hpp, the wire-c settings rds.log / rds.log.folder / rds.log.name, engine_wire_c.cpp).
//
//   R1  OFF by default: with the decoder's snapshot changing and no switch, nothing is written and no file or folder is made
//   R2  ON with a folder: a change of the station name, the programme type or the radio text is one line of a CSV in that folder, written off the control thread; the text only: no frequency,
//       no PI code; the facts say how many lines (never what they said)
//   R3  the settings are refused with the recorder's own words: a name that leaves the folder, a folder that is not a full path
//   R4  switched OFF again: no more lines
//
// The decoder's snapshot is the engine's own test seam (setting test.rds), so the whole chain from the publish pass to the file is the real one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

void setRds(Rig& r, const std::string& value) {
    const testing::Outcome o = r.send(testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, ("test.rds=" + value).c_str()));
    CHECK(o.landed && o.result.status == FOXAPI_OK);
    r.pass(3);
}

std::string onlyFile(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    int n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file()) { all = readAll(e.path()); ++n; }
    }
    return n == 1 ? all : std::string("<files:" + std::to_string(n) + ">");
}

bool waitLines(Rig& r, const std::string& want) {
    for (int i = 0; i < 200; ++i) {
        r.pass(2);
        if (r.fact("rds.log.lines") == want) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("rds.log.lines=%s error=%s dropped=%s\n", r.fact("rds.log.lines").c_str(), r.fact("rds.log.error").c_str(), r.fact("rds.log.dropped").c_str());
    return false;
}

void testOffByDefault(const fs::path& dir) {
    const fs::path logs = dir / "rdslogs";
    Rig r(dir / "u1");
    CHECK(r.fact("wirec.rds.log") == "0");
    CHECK(r.fact("wirec.rds.log.folder").empty());
    CHECK(r.fact("rds.log.lines") == "0");
    setRds(r, "SPNTA;C201;10;321;12;BBC R4  ;Now: the news");
    setRds(r, "SPNTA;C201;10;321;12;LBC     ;Phone in");
    r.pass(5);
    CHECK(r.fact("rds.log.lines") == "0");
    CHECK(!fs::exists(logs));
}

void testWritesTextOnly(const fs::path& dir) {
    const fs::path logs = dir / "rdslogs2";
    Rig r(dir / "u2");
    CHECK(r.status(setting("rds.log.folder", logs.string())) == FOXAPI_OK);
    CHECK(r.status(setting("rds.log", "1")) == FOXAPI_OK);
    r.pass(3);
    CHECK(r.fact("rds.log.error").empty());
    setRds(r, "SPNTA;C201;10;321;12;BBC R4  ;Now: the news");   // pty 10 = Pop music, a name, a text: one line
    CHECK(waitLines(r, "1"));
    setRds(r, "SPNTA;C201;10;322;12;BBC R4  ;Now: the news");   // only the counters moved: no line
    r.pass(4);
    CHECK(r.fact("rds.log.lines") == "1");
    setRds(r, "SPNTA;C201;4;323;12;BBC R4  ;Sport at five");     // the type and the text changed: one line
    CHECK(waitLines(r, "2"));
    setRds(r, "SPNTA;C201;4;324;12;LBC     ;Sport at five");    // the name changed: one line
    CHECK(waitLines(r, "3"));
    const std::string file = onlyFile(logs);
    CHECK(file.rfind("time_utc,radio,station_name,programme_type_code,programme_type,radio_text\n", 0) == 0 || file.rfind("time_utc,radio,station_name,programme_type_code,programme_type,radio_text\r\n", 0) == 0);
    CHECK(file.find(",1,BBC R4,10,Pop music,Now: the news") != std::string::npos);
    CHECK(file.find(",1,BBC R4,4,Sport,Sport at five") != std::string::npos);
    CHECK(file.find(",1,LBC,4,Sport,Sport at five") != std::string::npos);
    CHECK(file.find("C201") == std::string::npos && file.find("c201") == std::string::npos);   // no PI code
    CHECK(file.find("100.") == std::string::npos);                                               // no frequency (the generator's centre is near 100 MHz)
    // the facts name counts and a sentence, never what was written
    CHECK(r.fact("rds.log.lines").find("BBC") == std::string::npos);
    CHECK(r.fact("rds.log.dropped") == "0");
    // switched off: no more lines
    CHECK(r.status(setting("rds.log", "0")) == FOXAPI_OK);
    r.pass(3);
    setRds(r, "SPNTA;C201;4;325;12;KISS    ;Other words");
    r.pass(6);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(onlyFile(logs) == file);
}

void testRefusals(const fs::path& dir) {
    Rig r(dir / "u3");
    std::string msg;
    CHECK(r.status(setting("rds.log.name", "../escape"), &msg) == FOXAPI_BAD_ARGUMENT);
    CHECK(!msg.empty());
    CHECK(r.status(setting("rds.log.name", "bad:name"), &msg) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("rds.log.name", "{nonsense}"), &msg) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("rds.log.folder", "relative/path"), &msg) == FOXAPI_BAD_ARGUMENT);
    CHECK(msg.find("full path") != std::string::npos);
    CHECK(r.status(setting("rds.log.name", "rds_{date}_{radio}")) == FOXAPI_OK);
    CHECK(r.fact("wirec.rds.log.name") == "rds_{date}_{radio}");
    // on with no folder: said, nothing written
    CHECK(r.status(setting("rds.log", "1")) == FOXAPI_OK);
    r.pass(3);
    CHECK(!r.fact("rds.log.error").empty());
    setRds(r, "SPNTA;C201;10;321;12;BBC R4  ;x");
    r.pass(4);
    CHECK(r.fact("rds.log.lines") == "0");
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_rds");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("rds");
    testOffByDefault(dir);
    testWritesTextOnly(dir);
    testRefusals(dir);
    return check::finish("test_engine_api_wire_c_rds");
}
