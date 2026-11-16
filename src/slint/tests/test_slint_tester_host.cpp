// LANE X, row WG2-1492 (docs/multi-radio-records/lane-x.md): the Slint host's tester usage recorder (tester_host.cpp), without an engine: facts in, marks and the masked preview out.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tester_host.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"

using namespace foxsdr::slinttester;
using nlohmann::json;

namespace {

const std::string kToken = "0123456789abcdef0123456789abcdef";
const std::time_t kNow = 1790000000;   // 2026-09-21

bool has(const json& a, const std::string& v) { return std::find(a.begin(), a.end(), json(v)) != a.end(); }

void notArmed() {
    TesterDuty d;
    CHECK(!d.armed() && d.previewJson(10.0, kNow).empty());
    Facts f;
    f.flags = FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_SCANNER_ACTIVE | FOXAPI_RX_TX_KEYED;
    f.sourceKind = "rtlsdr";
    d.note(f, 1.0);
    d.setToken(kToken, kNow, 2.0);     // armed only now: what was looked at before it is not in the record
    const json j = json::parse(d.report(62.0, kNow).toJson());
    CHECK(j["features"].empty() && j["radios"].empty());
}

void marks() {
    TesterDuty d;
    d.setToken(kToken, kNow, 0.0);
    CHECK(d.armed());
    Facts f;
    f.flags = FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_RECORDING_AUDIO | FOXAPI_RX_SCANNER_ACTIVE | FOXAPI_RX_DEVICE_OPEN;
    f.sourceKind = "rtlsdr";
    f.webServed = true;
    d.note(f, 1.0);
    const json j = json::parse(d.report(61.0, kNow).toJson());
    CHECK(j["token"] == kToken && j["session"]["minutes"] == 1);
    const json& ft = j["features"];
    CHECK(has(ft, "rec-iq") && has(ft, "rec-audio") && has(ft, "scanner") && has(ft, "browser") && has(ft, "native-drivers"));
    CHECK(!has(ft, "transmit") && !has(ft, "cat") && !has(ft, "siggen"));
    CHECK(has(j["radios"], "rtlsdr"));
    // a soapy radio is a radio but not a native driver; a file is not a radio; the generator is a feature only while the receiver runs
    TesterDuty e;
    e.setToken(kToken, kNow, 0.0);
    Facts g;
    g.flags = FOXAPI_RX_DEVICE_OPEN;
    g.sourceKind = "soapy";
    e.note(g, 1.0);
    g.sourceKind = "file";
    e.note(g, 2.0);
    g.sourceKind = "siggen";
    e.note(g, 3.0);                      // not running: nothing
    const json k = json::parse(e.report(10.0, kNow).toJson());
    CHECK(has(k["radios"], "soapy") && !has(k["radios"], "file") && !has(k["features"], "native-drivers") && !has(k["features"], "siggen"));
    g.flags |= FOXAPI_RX_RUNNING;
    e.note(g, 4.0);
    CHECK(has(json::parse(e.report(10.0, kNow).toJson())["features"], "siggen"));
    // the transmitter keyed is marked as the fact says, and nothing here can key it
    g.flags |= FOXAPI_RX_TX_KEYED;
    e.note(g, 5.0);
    CHECK(has(json::parse(e.report(10.0, kNow).toJson())["features"], "transmit"));
}

void plugins() {
    TesterDuty d;
    d.setToken(kToken, kNow, 0.0);
    Facts f;
    f.fed = {{"aprs", "1.2.0"}, {"sideloaded", ""}};
    d.note(f, 0.0);                      // the first look credits nothing (no time has passed since the arming)
    d.note(f, 60.0);                     // one minute of both
    f.fed = {{"aprs", "1.2.0"}};
    d.note(f, 150.0);                    // 90 s more of aprs only
    const json j = json::parse(d.report(150.0, kNow).toJson());
    const json& pl = j["plugins"];
    CHECK(pl.size() == 2);
    for (const json& p : pl) {
        if (p["id"] == "aprs") { CHECK(p["version"] == "1.2.0" && p["minutes"] == 3); }   // 150 s = 2.5 min, rounded
        else { CHECK(p["id"] == "sideloaded" && p["version"] == "" && p["minutes"] == 1); }
    }
    // a gap longer than two minutes (the window was asleep) is credited as two minutes, not as the whole gap
    TesterDuty e;
    e.setToken(kToken, kNow, 0.0);
    Facts g;
    g.fed = {{"aprs", "1.2.0"}};
    e.note(g, 0.0);
    e.note(g, 3600.0);
    CHECK(json::parse(e.report(3600.0, kNow).toJson())["plugins"][0]["minutes"] == 2);
    // a decoder node of a patch is the "plugins" feature
    Facts h;
    h.patchDecoder = true;
    e.note(h, 3601.0);
    CHECK(has(json::parse(e.report(3601.0, kNow).toJson())["features"], "plugins"));
}

void preview() {
    TesterDuty d;
    d.setToken(kToken, kNow, 0.0);
    Facts f;
    f.sourceKind = "siggen";
    f.flags = FOXAPI_RX_RUNNING;
    d.note(f, 1.0);
    const std::string p = d.previewJson(30.0, kNow);
    CHECK(!p.empty() && p.find(kToken) == std::string::npos);        // the preview is MASKED: the whole code is never on the screen
    CHECK(p.find("0123...cdef") != std::string::npos);
    const json j = json::parse(p);
    CHECK(has(j["features"], "siggen") && j["platform"] == "windows" && !j["version"].get<std::string>().empty());
    // what is never in it: no frequency, position, file name or decoded text field exists in the report's shape
    const std::string compact = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    for (const char* bad : {"freq", "lat", "lon", "path", "file", "text", "call"}) { CHECK(compact.find(std::string("\"") + bad) == std::string::npos); }
    // taking the code away empties it, and the next code starts a fresh record
    d.setToken("", kNow, 40.0);
    CHECK(!d.armed() && d.previewJson(41.0, kNow).empty());
    d.setToken(kToken, kNow, 100.0);
    CHECK(json::parse(d.report(100.0, kNow).toJson())["features"].empty());
}

}  // namespace

int main() {
    notArmed();
    marks();
    plugins();
    preview();
    return testSummary("test_slint_tester_host");
}
