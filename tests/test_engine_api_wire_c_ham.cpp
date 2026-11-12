// test_engine_api_wire_c_ham.cpp - the QSO log, the Cabrillo export and the ADIF export of wiring lane C in the real engine (src/engine/wire_c_ham.{hpp,cpp}, engine_wire_c.cpp; models
// core/qso_log, cabrillo, adif_qso). A LOG the person keeps: nothing here transmits.
//
//   Q1  QSO_ADD: good QSOs are logged (count fact); every refusal says why and logs nothing (no time, no frequency, no mode, a bad call sign, a bad exchange, not JSON); the log survives a
//       new engine on the same folder; QSO_REMOVE and QSO_CLEAR (NOT_FOUND / NO_CHANGE for nothing to do)
//   Q2  CABRILLO: the header is kept; the export is a real Cabrillo log (START-OF-LOG, the contest and call sign, one QSO line each in time order, END-OF-LOG); a header the model refuses
//       (an unknown contest, no call sign) writes NOTHING and lists the problems; a file that cannot be written is refused
//   Q3  ADIF: a header and one record per QSO that can be written; nothing writable is refused
//   Q4  PRIVACY: no fact carries a call sign, a frequency or an exchange
//   Q5  a REMOTE session cannot send any of the ops
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

FoxCommand textCmd(std::uint32_t op, const std::string& text, std::int64_t a = 0) { return testing::cmd(op, 0.0, a, text.c_str()); }

std::string qso(std::int64_t t, double khz, const char* mode, const char* call, const char* me = "G4XYZ", const char* sent = "[\"599\",\"14\"]", const char* rcvd = "[\"579\",\"5\"]") {
    return "{\"t\":" + std::to_string(t) + ",\"f\":" + std::to_string(khz) + ",\"m\":\"" + mode + "\",\"c\":\"" + call + "\",\"me\":\"" + me + "\",\"s\":" + sent + ",\"r\":" + rcvd + "}";
}

const char* kHeader =
    "{\"contest\":\"CQ-WW-CW\",\"callsign\":\"G4XYZ\",\"location\":\"DX\",\"name\":\"A. Listener\",\"address\":[\"1 Example Lane\"],\"operators\":[\"G4XYZ\"],\"soapbox\":[\"Heard a lot\"],\"score\":1234}";

void testLog(const fs::path& dir) {
    {
        Rig r(dir);
        CHECK(r.fact("ham.qso.count") == "0");
        // refusals first: nothing is logged by any of them
        const std::vector<std::string> bad = {"not json",
                                              qso(1790000000, 14025.0, "CW", "K1 ABC"),
                                              qso(1790000000, 14025.0, "CW", ""),
                                              qso(1790000000, 0.0, "CW", "K1ABC"),
                                              qso(1790000000, 14025.0, "", "K1ABC"),
                                              qso(1790000000, 14025.0, "CW", "K1ABC", "G4XYZ", "[\"59 9\"]"),
                                              qso(-5, 14025.0, "CW", "K1ABC"),
                                              qso(1790000000, 14025.0, "CW", "K1ABC", "G4 XYZ")};
        for (const std::string& b : bad) {
            CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, b)) == FOXAPI_BAD_ARGUMENT);
            CHECK(!r.fact("ham.error").empty());
        }
        CHECK(r.fact("ham.qso.count") == "0");
        CHECK(!fs::exists(dir / "qso-log.json"));
        // good ones (out of time order on purpose)
        CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000300, 14025.0, "CW", "K1ABC"))) == FOXAPI_OK);
        CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000100, 7010.5, "cw", "DL1XYZ"))) == FOXAPI_OK);
        CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000200, 21030.0, "CW", "JA1ABC/P"))) == FOXAPI_OK);
        CHECK(r.fact("ham.qso.count") == "3");
        CHECK(r.status(textCmd(FOXAPP_OP_QSO_REMOVE, "", 9)) == FOXAPI_NOT_FOUND);
        CHECK(r.status(textCmd(FOXAPP_OP_QSO_REMOVE, "", 0)) == FOXAPI_NOT_FOUND);
    }
    {
        Rig again(dir);   // the log survives a new engine
        CHECK(again.fact("ham.qso.count") == "3");
        CHECK(again.status(textCmd(FOXAPP_OP_QSO_CLEAR, "")) == FOXAPI_OK);
        CHECK(again.fact("ham.qso.count") == "0");
        CHECK(again.status(textCmd(FOXAPP_OP_QSO_CLEAR, "")) == FOXAPI_NO_CHANGE);
        CHECK(again.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000300, 14025.0, "CW", "K1ABC"))) == FOXAPI_OK);
        CHECK(again.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000400, 14026.0, "CW", "W2XYZ"))) == FOXAPI_OK);
        CHECK(again.status(textCmd(FOXAPP_OP_QSO_REMOVE, "", 1)) == FOXAPI_OK);
        CHECK(again.fact("ham.qso.count") == "1");
    }
}

void testCabrilloAndAdif(const fs::path& dir) {
    Rig r(dir);
    // no header yet: the model refuses and writes nothing
    CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000300, 14025.0, "CW", "K1ABC"))) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000100, 7010.5, "CW", "DL1XYZ"))) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000200, 21030.0, "CW", "JA1ABC/P"))) == FOXAPI_OK);
    const fs::path out = dir.parent_path() / "log.cbr";
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_EXPORT, out.string())) == FOXAPI_BAD_ARGUMENT);
    CHECK(!fs::exists(out));
    CHECK(!r.fact("ham.cabrillo.problems").empty());
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_HEADER, "not json")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_HEADER, "{\"contest\":\"NO-SUCH-CONTEST\",\"callsign\":\"G4XYZ\"}")) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_EXPORT, out.string())) == FOXAPI_BAD_ARGUMENT);   // the contest's template was not read: no QSO lines can be laid out
    CHECK(!fs::exists(out));
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_HEADER, "{\"contest\":\"CQ-WW-CW\",\"callsign\":\"G4 XYZ\"}")) == FOXAPI_BAD_ARGUMENT);
    // a good header
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_HEADER, kHeader)) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_EXPORT, out.string())) == FOXAPI_OK);
    const std::string cab = readAll(out);
    CHECK(cab.find("START-OF-LOG:") == 0u);
    CHECK(cab.find("CONTEST: CQ-WW-CW") != std::string::npos);
    CHECK(cab.find("CALLSIGN: G4XYZ") != std::string::npos);
    CHECK(cab.find("END-OF-LOG:") != std::string::npos);
    // three QSO lines, in chronological order (DL1XYZ at ...100, JA1ABC/P at ...200, K1ABC at ...300)
    int qsoLines = 0;
    std::size_t at = 0;
    std::size_t prev = 0;
    bool ordered = true;
    while ((at = cab.find("QSO:", at)) != std::string::npos) {
        ++qsoLines;
        ++at;
        (void)prev;
    }
    CHECK(qsoLines == 3);
    CHECK(cab.find("DL1XYZ") < cab.find("JA1ABC/P") && cab.find("JA1ABC/P") < cab.find("K1ABC"));
    (void)ordered;
    CHECK(r.fact("ham.export.last").find("3 QSO line(s) written") != std::string::npos);
    // a file that cannot be written
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_EXPORT, (dir.parent_path() / "no-such-folder" / "x" / "y.cbr").string())) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_CABRILLO_EXPORT, "")) == FOXAPI_BAD_ARGUMENT);

    // ADIF
    const fs::path adi = dir.parent_path() / "log.adi";
    CHECK(r.status(textCmd(FOXAPP_OP_ADIF_EXPORT, adi.string())) == FOXAPI_OK);
    const std::string adif = readAll(adi);
    CHECK(adif.find("<EOH>") != std::string::npos);
    int records = 0;
    for (std::size_t p = 0; (p = adif.find("<EOR>", p)) != std::string::npos; ++p) { ++records; }
    CHECK(records == 3);
    CHECK(adif.find("<CALL:5>K1ABC") != std::string::npos);
    CHECK(adif.find("<STATION_CALLSIGN:5>G4XYZ") != std::string::npos);
    CHECK(r.fact("ham.export.last").find("ADIF: 3 record(s) written") != std::string::npos);
    // nothing writable: refused
    CHECK(r.status(textCmd(FOXAPP_OP_QSO_CLEAR, "")) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_ADIF_EXPORT, (dir.parent_path() / "empty.adi").string())) == FOXAPI_BAD_ARGUMENT);
    CHECK(!fs::exists(dir.parent_path() / "empty.adi"));

    // Q4: no fact carries a call sign, a frequency or an exchange
    CHECK(r.status(textCmd(FOXAPP_OP_QSO_ADD, qso(1790000300, 14025.0, "CW", "K1ABC"))) == FOXAPI_OK);
    for (const char* key : {"ham.qso.count", "ham.error", "ham.log.error", "ham.export.last", "ham.cabrillo.problems", "ham.cabrillo.contests"}) {
        const std::string v = r.fact(key);
        CHECK(v.find("K1ABC") == std::string::npos && v.find("G4XYZ") == std::string::npos && v.find("14025") == std::string::npos);
    }
    CHECK(r.fact("ham.cabrillo.contests").find("CQ-WW-CW") != std::string::npos);
}

void testRemote(const fs::path& dir) {
    Rig r(dir);
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    for (const std::uint32_t op : {FOXAPP_OP_QSO_ADD, FOXAPP_OP_QSO_REMOVE, FOXAPP_OP_QSO_CLEAR, FOXAPP_OP_CABRILLO_HEADER, FOXAPP_OP_CABRILLO_EXPORT, FOXAPP_OP_ADIF_EXPORT}) {
        CHECK_EQ(r.sendOn(remote, textCmd(op, qso(1790000300, 14025.0, "CW", "K1ABC"), 1)).status(), FOXAPI_DENIED);
    }
    CHECK(r.fact("ham.qso.count") == "0");
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_ham");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("ham");
    int n = 0;
    for (auto fn : {testLog, testCabrilloAndAdif, testRemote}) {
        const fs::path d = root / std::to_string(n++) / "foxsdr";
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_ham");
}
