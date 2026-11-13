// Screen lane L's pure rules (src/slint/lanel_logic.hpp; docs/multi-radio-records/screen-l.md): what a typed field means, the JSON text of each engine operation, the readers of results, the words of a
// refused answer and the needed-libraries panel (nine rows, seven states, "Found (not test-loaded)" and never "Working" for a header-only check).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"
#include "lanel_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slintlanel;
using nlohmann::json;

namespace {

bool hasRow(const std::vector<Row>& rows, int kind, const std::string& label) {
    for (const auto& r : rows) { if (r.kind == kind && r.label == label) { return true; } }
    return false;
}
std::string valueOf(const std::vector<Row>& rows, const std::string& label) {
    for (const auto& r : rows) { if (r.label == label) { return r.value; } }
    return "<none>";
}

// a stand-in for the engine's needed.libs fact: three rows in three shapes (a copy-in file checked by headers, the loaded FTDI installer, a program that cannot be checked)
const char* kNeeded = R"JSON([
 {"library_id":"winradio-g39ddc","source_kind":"g39ddc","name":"WiNRADiO G39DDC driver","kind":"library-file","file_names":["G39DDCAPI.dll"],"why":"Needed to open the G39DDC.","publisher":"WiNRADiO","official_page":"","page_status":"unverified","install_kind":"copy-in","steps":["Get it from the maker.","Copy it in."],"licence":"The maker's licence.","not_supplied":"FoxSDR does not include or supply it.","fetch_allowed":"no (unverified)","check":{"state":"working","sentence":"found, 64-bit, well formed","depth":"headers","found_in":"C:/x","file":"G39DDCAPI.dll","version":""}},
 {"library_id":"ftdi-d2xx","source_kind":"sdriq","name":"FTDI D2XX driver","kind":"library-file","file_names":["ftd2xx.dll"],"why":"The SDR-IQ is read through it.","publisher":"FTDI","official_page":"https://ftdichip.com","page_status":"verified","install_kind":"installer","steps":["Run the installer."],"licence":"FTDI's licence.","not_supplied":"FoxSDR does not include or supply it.","fetch_allowed":"no (unverified)","check":{"state":"working","sentence":"found and loaded","depth":"loaded","version":"2.12.36"}},
 {"library_id":"sdrplay-sdrconnect","source_kind":"sdrconnect","name":"SDRconnect","kind":"running-program","file_names":[],"why":"A program that must be running.","publisher":"SDRplay","official_page":"https://example.invalid/sdrconnect","page_status":"unverified","install_kind":"program","steps":[],"licence":"","not_supplied":"FoxSDR does not include or supply it.","fetch_allowed":"no (unverified)","check":{"state":"cannot-check","sentence":"FoxSDR cannot see a program that is running; opening the radio says whether it could connect.","depth":""}},
 {"library_id":"soapy-bladerf","source_kind":"","name":"BladeRF module","kind":"soapy-module","file_names":["bladeRFSupport.dll"],"why":"x","publisher":"","official_page":"","page_status":"unverified","install_kind":"copy-in","steps":[],"licence":"","not_supplied":"FoxSDR does not include or supply it.","fetch_allowed":"no (unverified)","check":null}
])JSON";

}  // namespace

int main() {
    // ---------------------------------------------------------------- typed text
    double d = 0; long long n = 0; std::string e;
    CHECK(parseNumber("  12.5 ", 0, 100, d, e) && d == 12.5);
    CHECK(parseNumber("+3", 0, 100, d, e) && d == 3.0);
    CHECK(!parseNumber("", 0, 100, d, e) && !e.empty());
    CHECK(!parseNumber("12,5", 0, 100, d, e));                  // a comma is not a decimal point here
    CHECK(!parseNumber("12abc", 0, 100, d, e));
    CHECK(!parseNumber("nan", 0, 100, d, e));
    CHECK(!parseNumber("inf", 0, 1e300, d, e));
    CHECK(!parseNumber("101", 0, 100, d, e) && e.find("100") != std::string::npos);   // the sentence names the range
    CHECK(!parseNumber("-0.001", 0, 100, d, e));
    CHECK(parseNumber("100", 0, 100, d, e) && d == 100.0);      // the boundary is inside
    CHECK(parseInteger("42", 1, 100, n, e) && n == 42);
    CHECK(!parseInteger("4.2", 1, 100, n, e));
    CHECK(!parseInteger("0", 1, 100, n, e));
    CHECK(!parseInteger("99999999999999999999", 1, 100, n, e));
    CHECK(numberText(1.5) == "1.5" && numberText(7) == "7" && numberText(0.000001) == "1e-06");
    std::vector<std::string> items;
    CHECK(splitList("a, b;c\n d ,,", 10, 5, items) && items.size() == 4 && items[3] == "d");
    CHECK(!splitList("toolong", 10, 3, items));
    CHECK(splitList("a,b,c,d", 2, 5, items) && items.size() == 2);
    std::string com;
    CHECK(validComPort(" com7 ", com) && com == "COM7");
    CHECK(validComPort("COM256", com) && com == "COM256");
    CHECK(!validComPort("COM257", com) && !validComPort("COM0", com) && !validComPort("COM", com) && !validComPort("COM01", com) && !validComPort("LPT1", com) && !validComPort("COM1;calc", com));

    // ---------------------------------------------------------------- operation texts
    {
        const Built b = scanTriggerJson(1, "-70", "8", "2", "0.1");
        CHECK(b.ok);
        const json j = json::parse(b.text);
        CHECK(j["mode"] == "relative" && j["version"] == 1 && j["marginDb"] == 8.0 && j["riseDbPerReading"] == 0.1);
        CHECK(json::parse(scanTriggerJson(0, "-70", "8", "2", "0.1").text)["mode"] == "fixed");
        CHECK(!scanTriggerJson(1, "-170", "8", "2", "0.1").ok);   // level below the engine's -160
        CHECK(!scanTriggerJson(1, "-70", "61", "2", "0.1").ok);
        CHECK(!scanTriggerJson(1, "-70", "8", "31", "0.1").ok);
        CHECK(!scanTriggerJson(1, "-70", "8", "2", "11").ok);
        CHECK(scanTriggerJson(1, "-170", "8", "2", "0.1").err.find("Level") != std::string::npos);   // the sentence names the field
    }
    {
        const Built t = scanTriggerJson(1, "-70", "8", "2", "0.1");
        const Built p = scanPlanJson("144 146 12.5 nfm 12.5 two metres; 430 440 25 NFM 25", 1, t.text);
        CHECK(p.ok);
        const json j = json::parse(p.text);
        CHECK(j["direction"] == "backward" && j["ranges"].size() == 2);
        CHECK(j["ranges"][0]["startHz"] == 144e6 && j["ranges"][0]["stepHz"] == 12500.0 && j["ranges"][0]["mode"] == "NFM" && j["ranges"][0]["label"] == "two metres");
        CHECK(j["ranges"][1]["label"] == "" && j["trigger"]["mode"] == "relative");
        CHECK(!scanPlanJson("", 0, t.text).ok);
        CHECK(!scanPlanJson("144 146 12.5 NFM", 0, t.text).ok);          // a word missing
        CHECK(scanPlanJson("144 146 12.5 NFM", 0, t.text).err.find("Range 1") != std::string::npos);
        CHECK(!scanPlanJson("146 144 12.5 NFM 12.5", 0, t.text).ok);     // the stop below the start
        CHECK(!scanPlanJson("144 146 abc NFM 12.5", 0, t.text).ok);
        std::string seventeen;
        for (int i = 0; i < 17; ++i) { seventeen += "144 145 25 NFM 12.5;"; }
        CHECK(!scanPlanJson(seventeen, 0, t.text).ok);                    // 17 ranges
        CHECK(!scanPlanJson("144 146 12.5 NFM 12.5", 0, "not json").ok);
    }
    {
        const Built b = fftScanJson(true, "88", "108", "100", 0, "8", "-70", true);
        CHECK(b.ok);
        const json j = json::parse(b.text);
        CHECK(j["enabled"] == true && j["startHz"] == 88e6 && j["stepHz"] == 100000.0 && j["mode"] == "relative" && j["merge"] == true);
        CHECK(!fftScanJson(true, "108", "88", "100", 0, "8", "-70", true).ok);
        CHECK(!fftScanJson(true, "88", "108", "0", 0, "8", "-70", true).ok);
        CHECK(json::parse(fftScanJson(false, "88", "108", "100", 1, "8", "-70", false).text)["mode"] == "fixed");
    }
    {
        const json a = json::parse(bandMemoryJson(true, true, false).text);
        CHECK(a["enabled"] == true && a["fields"].size() == 1 && a["fields"][0] == "volume");
        CHECK(json::parse(bandMemoryJson(false, false, false).text)["fields"].empty());
        CHECK(json::parse(recordDailyJson(3, false).text)["style"] == 3 && json::parse(recordDailyJson(3, false).text)["utc"] == false);
        CHECK(!recordDailyJson(5, true).ok && !recordDailyJson(-1, true).ok);
    }
    {
        const Built b = overlayLoadJson("C:/data/stations.csv", 2, "frequency", "name, location", 1);
        CHECK(b.ok);
        const json j = json::parse(b.text);
        CHECK(j["path"] == "C:/data/stations.csv" && j["config"]["unit"] == "MHz" && j["config"]["delimiter"] == ";" && j["config"]["columns"].size() == 2 && j["config"]["version"] == 1);
        CHECK(json::parse(overlayLoadJson("p", 0, "f", "a", 2).text)["config"]["delimiter"] == "\t");
        CHECK(!overlayLoadJson("", 2, "frequency", "name", 0).ok && !overlayLoadJson("p", 2, "frequency", "", 0).ok && !overlayLoadJson("p", 2, "", "a", 0).ok);
        std::string many;
        for (int i = 0; i < 17; ++i) { many += "c" + std::to_string(i) + ","; }
        CHECK(overlayLoadJson("p", 2, "f", many, 0).ok && json::parse(overlayLoadJson("p", 2, "f", many, 0).text)["config"]["columns"].size() == 16);   // capped at 16
    }
    {
        CHECK(packExportJson("C:/p.json", "My pack", "d").ok && !packExportJson("", "n", "").ok && !packExportJson("p", "", "").ok && !packExportJson("p", std::string(65, 'x'), "").ok);
        CHECK(favouriteSaveJson(" Air ").ok && json::parse(favouriteSaveJson(" Air ").text)["name"] == "Air" && !favouriteSaveJson("").ok && !favouriteSaveJson(std::string(65, 'x')).ok);
        const json nj = json::parse(bookmarkNoteJson("a note", "x, y").text);
        CHECK(nj["note"] == "a note" && nj["tags"].size() == 2);
        CHECK(!bookmarkNoteJson(std::string(2001, 'a'), "").ok && !bookmarkNoteJson("n", std::string(33, 't')).ok);
        CHECK(json::parse(bookmarkNoteJson("", "").text)["tags"].empty());     // the engine reads an empty note and no tags as "remove"
        const json f = json::parse(bookmarkFieldsJson("-90", "", "", "88.5").text);
        CHECK(f["squelchDb"] == -90.0 && f["toneHz"] == 88.5 && !f.contains("thresholdDb") && !f.contains("offsetKhz"));   // a left-out field is "none"
        CHECK(json::parse(bookmarkFieldsJson("", "", "", "").text).empty());
        CHECK(!bookmarkFieldsJson("-201", "", "", "").ok && !bookmarkFieldsJson("", "51", "", "").ok && !bookmarkFieldsJson("", "", "", "0").ok);
        const json r = json::parse(recordingsListJson("fm", 2).text);
        CHECK(r["sort"] == "start" && r["text"] == "fm" && r["limit"] == 50);
    }
    {
        const json s = json::parse(sigmeasJson("0.25", "0").text);
        CHECK(s["duration_s"] == 0.25 && s["end_ago_s"] == 0.0);
        CHECK(!sigmeasJson("0", "0").ok && !sigmeasJson("10.01", "0").ok && !sigmeasJson("1", "61").ok && sigmeasJson("10", "60").ok);
        CHECK(sigmeasJson("0", "0").err.find("Length") != std::string::npos);
        CHECK(json::parse(pseudoDopplerJson("100", "0.25").text)["samples_per_dwell"] == 100);
        CHECK(!pseudoDopplerJson("7", "0.25").ok && !pseudoDopplerJson("1000001", "0.25").ok);
        const json m = json::parse(maskConfigJson(true, "3", "5", false, "2", "10").text);
        CHECK(m["enabled"] == true && m["raise_frames"] == 3 && m["clear_frames"] == 5 && m["capture"] == false && m["post_roll_s"] == 2.0 && m["min_interval_s"] == 10.0);
        CHECK(!maskConfigJson(true, "0", "5", false, "2", "10").ok && !maskConfigJson(true, "3", "101", false, "2", "10").ok && !maskConfigJson(true, "3", "5", false, "61", "10").ok && !maskConfigJson(true, "3", "5", false, "2", "3601").ok);
    }
    {   // the sweep: four kinds, each with its own keys
        const json t = json::parse(sweepOpenJson(0, "com3", "", "88", "108", "450", "0", "", "", "", false, "", "").text);
        CHECK(t["kind"] == "tinysa" && t["port"] == "COM3" && t["points"] == 450 && t["start_mhz"] == 88.0 && t["interval_ms"] == 0);
        CHECK(json::parse(sweepOpenJson(1, "COM3", "", "88", "108", "450", "0", "", "", "", false, "", "").text)["kind"] == "tinysa_ultra");
        CHECK(!sweepOpenJson(0, "", "", "88", "108", "450", "0", "", "", "", false, "", "").ok);            // no port, nothing opens
        CHECK(!sweepOpenJson(0, "COM3", "", "108", "88", "450", "0", "", "", "", false, "", "").ok);
        CHECK(!sweepOpenJson(0, "COM3", "", "88", "108", "1", "0", "", "", "", false, "", "").ok);          // 1 point
        const Built h = sweepOpenJson(2, "ignored", "", "88", "108", "", "", "10", "16", "16", true, "", "");
        const json hj = json::parse(h.text);
        CHECK(h.ok && hj["kind"] == "hackrf" && !hj.contains("port") && hj["step_mhz"] == 10.0 && hj["amp"] == true && hj["lna_db"] == 16.0);
        CHECK(!sweepOpenJson(2, "", "", "88.5", "108", "", "", "10", "16", "16", false, "", "").ok);       // whole MHz only
        CHECK(!sweepOpenJson(2, "", "", "88", "7000", "", "", "10", "16", "16", false, "", "").ok);
        CHECK(!sweepOpenJson(2, "", "", "88", "108", "", "", "21", "16", "16", false, "", "").ok && !sweepOpenJson(2, "", "", "88", "108", "", "", "10", "41", "16", false, "", "").ok && !sweepOpenJson(2, "", "", "88", "108", "", "", "10", "16", "63", false, "", "").ok);
        const Built x = sweepOpenJson(3, "COM7", "500000", "149", "151", "", "", "", "", "", false, "-10", "-120");
        const json xj = json::parse(x.text);
        CHECK(x.ok && xj["kind"] == "rfexplorer" && xj["port"] == "COM7" && xj["baud"] == 500000 && xj["top_dbm"] == -10.0 && xj["bottom_dbm"] == -120.0);
        CHECK(!sweepOpenJson(3, "COM7", "250000", "149", "151", "", "", "", "", "", false, "-10", "-120").ok);   // not one of the nine baud rates
        CHECK(!sweepOpenJson(3, "COM7", "500000", "149", "151", "", "", "", "", "", false, "-120", "-10").ok);  // the bottom above the top
        CHECK(!sweepOpenJson(3, "COM7", "500000", "149", "151", "", "", "", "", "", false, "10", "-120").ok);   // the top above 0 dBm
        const json hm = json::parse(sweepHeatmapJson("1024", "200", 1, false, "", "", "", "", "", "").text);
        CHECK(hm["heatmap"]["cols"] == 1024 && hm["heatmap"]["freq_mode"] == "mean" && hm["occupancy"].is_null());
        const json oc = json::parse(sweepHeatmapJson("1024", "200", 0, true, "88", "108", "100", "10", "0.1", "-100").text);
        CHECK(oc["occupancy"]["start_mhz"] == 88.0 && oc["occupancy"]["step_khz"] == 100.0 && oc["occupancy"]["floor_percentile"] == 0.1);
        CHECK(!sweepHeatmapJson("0", "200", 0, false, "", "", "", "", "", "").ok && !sweepHeatmapJson("1024", "1", 0, false, "", "", "", "", "", "").ok);
        CHECK(!sweepHeatmapJson("1024", "200", 0, true, "88", "108", "100", "10", "0.6", "-100").ok);
    }
    {   // the outputs
        const json i = json::parse(iqOutJson(1, true, false, "0").text);
        CHECK(i["format"] == "s16" && i["header"] == true && i["udp"] == false && i["port"] == 0 && !i.contains("dest"));
        const json u = json::parse(iqOutJson(2, false, true, "5000").text);
        CHECK(u["udp"] == true && u["dest"] == "127.0.0.1" && u["format"] == "f32");   // never a wider destination
        CHECK(!iqOutJson(0, false, true, "0").ok && !iqOutJson(0, false, false, "65536").ok && !iqOutJson(0, false, false, "-1").ok);
        CHECK(json::parse(sbsOutJson("30003").text)["port"] == 30003 && !sbsOutJson("70000").ok);
        const json tr = json::parse(tracksConfigJson(true, "21600").text);
        CHECK(tr["enabled"] == true && tr["retention_s"] == 21600 && !tracksConfigJson(true, "59").ok && !tracksConfigJson(true, "2592001").ok);
        const Built cv = coverageConfigJson(true, "51.5", "-0.1", "0.05", "259200");
        const json cj = json::parse(cv.text);
        CHECK(cv.ok && cj["rx_lat"] == 51.5 && cj["rx_lon"] == -0.1 && cj["cell_deg"] == 0.05);
        CHECK(!coverageConfigJson(true, "", "", "0.05", "259200").ok);                        // enabling needs the person's own position
        CHECK(!json::parse(coverageConfigJson(false, "", "", "0.05", "259200").text).contains("rx_lat"));   // off needs none
        CHECK(!coverageConfigJson(true, "91", "0", "0.05", "259200").ok && !coverageConfigJson(true, "0", "181", "0.05", "259200").ok && !coverageConfigJson(true, "0", "0", "2", "259200").ok && !coverageConfigJson(true, "0", "0", "0.05", "10").ok);
    }
    {   // the ham operations
        const Built q = qsoJson("", "14025", "CW", "K1ABC", "G4XYZ", "599, 12", "599,14");
        const json qj = json::parse(q.text);
        CHECK(q.ok && qj["f"] == 14025.0 && qj["c"] == "K1ABC" && qj["me"] == "G4XYZ" && qj["s"].size() == 2 && qj["r"][1] == "14" && !qj.contains("t"));
        CHECK(json::parse(qsoJson("1700000000", "14025", "CW", "K1ABC", "", "", "").text)["t"] == 1700000000);
        CHECK(!json::parse(qsoJson("", "14025", "CW", "K1ABC", "", "", "").text).contains("me"));
        CHECK(!qsoJson("", "", "CW", "K1ABC", "", "", "").ok && !qsoJson("", "14025", "", "K1ABC", "", "", "").ok && !qsoJson("", "14025", "CW", "", "", "", "").ok && !qsoJson("x", "14025", "CW", "K1ABC", "", "", "").ok);
        const Built c = cabrilloHeaderJson("CQ-WW-CW", "G4XYZ", "DX", "A Person", "1 Street; Town", "G4XYZ, G4ABC", "");
        const json cj = json::parse(c.text);
        CHECK(c.ok && cj["contest"] == "CQ-WW-CW" && cj["address"].size() == 2 && cj["operators"].size() == 2 && cj["soapbox"].empty());
        CHECK(!cabrilloHeaderJson("", "", "", "", "", "", "").ok);
        CHECK(json::parse(neededCheckJson("ftdi-d2xx", "").text) == json({{"id", "ftdi-d2xx"}}));
        CHECK(json::parse(neededCheckJson("", "").text).empty());
        CHECK(json::parse(neededCheckJson("winradio-g39ddc", "C:/x/y.dll").text)["path"] == "C:/x/y.dll");
        const json eb = json::parse(exportBoxJson("C:/in.wav", "C:/out.wav", "0", "2", "98e6", "98.2e6", 2, 1).text);
        CHECK(eb["mode"] == "fm" && eb["format"] == "pcm16" && eb["t1"] == 2.0 && eb["hiHz"] == 98.2e6);
        CHECK(!exportBoxJson("", "C:/out.wav", "0", "2", "98e6", "98.2e6", 2, 1).ok && !exportBoxJson("a", "b", "2", "1", "98e6", "98.2e6", 2, 1).ok && !exportBoxJson("a", "b", "0", "2", "98.2e6", "98e6", 2, 1).ok);
    }
    {   // the radio list's ids
        CHECK(openRadioId(0, " 192.168.1.20 ").text == "open-rfspace:192.168.1.20");
        CHECK(openRadioId(11, "").text == "open-sdriq:" && openRadioId(11, "").ok);
        CHECK(sourceKinds().size() == 12 && sourceKindNames().size() == 12 && channelPlanIds().size() == 8);
        CHECK(!openRadioId(12, "x").ok && !openRadioId(-1, "x").ok && !openRadioId(0, std::string("a\nb")).ok && !openRadioId(0, std::string(201, 'a')).ok);
    }

    // ---------------------------------------------------------------- readers
    {
        const auto f = flattenJson(R"({"valid":true,"reason":"","result":{"bw_hz":12500.5,"list":[1,2],"deep":{"a":"x"},"big":[1,2,3,4,5,6]}})", 20);
        CHECK(f.size() >= 5);
        bool sawBw = false, sawList = false, sawDeep = false, sawBig = false, sawValid = false;
        for (const auto& kv : f) {
            if (kv.first == "result.bw_hz" && kv.second == "12500.5") { sawBw = true; }
            if (kv.first == "result.list" && kv.second == "1, 2") { sawList = true; }
            if (kv.first == "result.deep.a" && kv.second == "x") { sawDeep = true; }
            if (kv.first == "result.big" && kv.second == "6 items") { sawBig = true; }
            if (kv.first == "valid" && kv.second == "true") { sawValid = true; }
        }
        CHECK(sawBw && sawList && sawDeep && sawBig && sawValid);
        CHECK(flattenJson(R"({"a":1,"b":2,"c":3})", 2).size() == 2);        // capped
        CHECK(flattenJson("", 5).empty());
        const auto t = flattenJson("plain words", 5);
        CHECK(t.size() == 1 && t[0].second == "plain words");
        CHECK(flattenJson(std::string(1000, 'x'), 5)[0].second.size() == 300);   // a long non-JSON text is cut
        std::string big = "[";
        for (int i = 0; i < 100; ++i) { big += (i ? "," : "") + std::string("{\"x\":") + std::to_string(i) + "}"; }
        CHECK(flattenJson(big + "]", 6).size() <= 6);
        CHECK(jsonString(R"({"k":"v"})", "k") == "v" && jsonString(R"({"k":1})", "k").empty() && jsonString("bad", "k").empty());
        CHECK(jsonNumber(R"({"k":2.5})", "k", 9) == 2.5 && jsonNumber(R"({"k":"x"})", "k", 9) == 9);
        CHECK(jsonBool(R"({"k":true})", "k", false) && !jsonBool(R"({"k":1})", "k", false));
    }
    CHECK(!statusIsRefusal(FOXAPI_OK) && !statusIsRefusal(FOXAPI_NO_CHANGE) && statusIsRefusal(FOXAPI_BUSY) && statusIsRefusal(-1) && statusIsRefusal(5));
    CHECK(refusalText(FOXAPI_OK, "x").empty());
    CHECK(refusalText(FOXAPI_NO_CHANGE, "") == "Nothing changed");
    CHECK(refusalText(FOXAPI_BUSY, "") == "Busy");
    CHECK(refusalText(7, "The sample rate is locked while an I/Q recording is open.") == "Refused: The sample rate is locked while an I/Q recording is open.");   // the engine's own sentence comes through
    CHECK(refusalText(7, "  ") == "Refused");

    // ---------------------------------------------------------------- needed libraries
    {
        const std::vector<NeededRow> rows = parseNeeded(kNeeded);
        CHECK(rows.size() == 4);
        CHECK(rows[0].id == "winradio-g39ddc" && rows[0].checked && rows[0].depth == "headers" && rows[0].fileNames.size() == 1 && rows[0].steps.size() == 2);
        CHECK(neededStateWords(rows[0]) == "Found (not test-loaded)");               // a header-only "working" is NEVER "Working"
        CHECK(neededStateWords(rows[0]).find("working") == std::string::npos && neededStateWords(rows[0]).find("Working") == std::string::npos);
        CHECK(neededStateWords(rows[1]) == "Found and working");                     // only the loaded FTDI check may say so
        CHECK(neededStateWords(rows[2]) == "Cannot be checked from here");
        CHECK(!rows[3].checked && neededStateWords(rows[3]) == "Not checked yet");   // check: null
        CHECK(neededStateTone(rows[0]) == foxsdr::slintlanek::kDim);                 // not drawn in the good colour
        CHECK(neededStateTone(rows[1]) == foxsdr::slintlanek::kGood);
        CHECK(neededStateTone(rows[2]) == foxsdr::slintlanek::kDim && neededStateTone(rows[3]) == foxsdr::slintlanek::kDim);

        NeededRow r = rows[0];
        const char* states[7] = {"working", "not-found", "too-old", "wrong-32-bit", "would-not-load", "not-a-library", "cannot-check"};
        std::vector<std::string> words;
        for (const char* s : states) { r.state = s; r.depth = ""; words.push_back(neededStateWords(r)); }
        for (std::size_t i = 0; i < words.size(); ++i) { CHECK(!words[i].empty()); for (std::size_t k = i + 1; k < words.size(); ++k) { CHECK(words[i] != words[k]); } }   // seven different words
        r.state = "not-found"; CHECK(neededStateTone(r) == foxsdr::slintlanek::kWarn);
        r.state = "something-new"; CHECK(neededStateWords(r) == "Not checked yet");   // an unknown word is never read as good

        CHECK(neededTakesFile(rows[0]) && !neededTakesFile(rows[1]) && !neededTakesFile(rows[2]));   // the FTDI installer takes no file; a running program none

        std::vector<Row> out;
        neededRows(rows[0], "C:/dl/G39DDCAPI.dll", out);
        CHECK(hasRow(out, foxsdr::slintlanek::kKey, "Check again: WiNRADiO G39DDC driver") && hasRow(out, foxsdr::slintlanek::kKey, "Check this file: WiNRADiO G39DDC driver") && hasRow(out, foxsdr::slintlanek::kField, "Path of the file you downloaded: WiNRADiO G39DDC driver"));
        CHECK(valueOf(out, "State") == "Found (not test-loaded)");
        CHECK(valueOf(out, "Where to get it") == "from its maker (no address is known here)" || valueOf(out, "Where to get it") == "WiNRADiO");   // no address invented
        std::vector<Row> o1;
        neededRows(rows[1], "", o1);
        CHECK(!hasRow(o1, foxsdr::slintlanek::kKey, "Check this file: FTDI D2XX driver") && hasRow(o1, foxsdr::slintlanek::kKey, "Check again: FTDI D2XX driver"));                 // an installer: no file to choose
        CHECK(valueOf(o1, "State") == "Found and working" && valueOf(o1, "Version") == "2.12.36");
        CHECK(valueOf(o1, "Where to get it") == "https://ftdichip.com");                 // a verified address, as text
        std::vector<Row> o2;
        neededRows(rows[2], "", o2);
        CHECK(valueOf(o2, "Where to get it") == "https://example.invalid/sdrconnect (unverified)");   // an unverified address is marked
        CHECK(valueOf(o2, "State") == "Cannot be checked from here");
        for (const auto* set : {&out, &o1, &o2}) {
            bool supplied = false, anyLink = false;
            for (const auto& x : *set) {
                if (x.label == "FoxSDR does not include or supply it.") { supplied = true; }
                if (x.id.find("open") != std::string::npos || x.id.find("download") != std::string::npos || x.id.find("folder") != std::string::npos || x.id.find("install") != std::string::npos) { anyLink = true; }
            }
            CHECK(supplied && !anyLink);   // the line is always there; no key opens a page or a folder or installs
        }
        CHECK(parseNeeded("not json").empty() && parseNeeded("{}").empty() && parseNeeded("[1,2,{\"name\":\"x\"}]").empty());   // a row with no id is dropped
    }
    return testSummary("test_slint_lanel_host");
}
