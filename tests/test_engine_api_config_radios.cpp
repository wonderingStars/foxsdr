// test_engine_api_config_radios.cpp - THE SAVED SETTINGS OF RADIOS 2 TO 8, THE FILE'S SIDE (multi-radio engine, stage 5; docs/multi-radio-records/engine-stages-5-6.md).
//
//   A  ROUND TRIP with one radio (no `radios` key at all), two radios (one entry) and eight radios (seven): every field comes back, in order;
//   B  THE OLD FILE: a file shaped like the one 0.99.104 writes (no `radios`, no `pluginRadio`) loads as one radio with every setting it had, and writes back
//      without either key;
//   C  THE NEW FILE READ BY AN OLD BUILD: the file with eight radios is the old file plus exactly two keys (`radios`, `pluginRadio`) - schemaVersion is still 1,
//      and with those two keys taken away the document is the document the same settings wrote before they existed. An old reader ignores unknown keys
//      (config.hpp: "unknown fields -> ignored") and so reads radio 1 as it always did; it writes the file back without them (radios 2 to 8 are forgotten
//      there, radio 1 is not);
//   D  A DAMAGED ARRAY: the wrong type at the key, entries of the wrong type, fields of the wrong type, nine entries, repeated numbers, numbers that are none,
//      negative and absurd numbers, files and sound cards that cannot be a second radio, device arguments of a megabyte: nothing throws, radio 1 is whole, every
//      value is inside its range, at most seven radios survive and each has a distinct number from 2 to 8;
//   E  THE PLUGINS' RADIO (`pluginRadio`): the same per-entry rules as pluginRun.
//
// Each check is a break-it check of the rule it names (the record lists what was removed and what turned red).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "core/config.hpp"

using cascade::core::AppConfig;
using cascade::core::ConfigStore;
using cascade::core::SavedRadio;
using json = nlohmann::json;

namespace {

SavedRadio fullRadio(int number) {
    SavedRadio r;
    r.number = number;
    r.sourceKind = number % 2 == 0 ? "rtlsdr" : "soapy";
    r.args = number % 2 == 0 ? "serial=0000000" + std::to_string(number) : "driver=uhd,serial=3000" + std::to_string(number);
    r.sampleRateHz = 2.4e6 + 1000.0 * number;
    r.antenna = number % 2 == 0 ? "" : "RX2";
    r.toneHz = 1000.0 * number;
    r.centerHz = 98.0e6 + 1.0e6 * number;
    r.mode = number % 2 == 0 ? "NFM" : "USB";
    r.bandwidthHz = 12500.0 + number;
    r.vfoOffsetHz = -5000.0 * number;
    r.squelchDb = -40.0 - number;
    r.squelchAuto = number % 3 == 0;
    r.volume = 0.1 * number;
    r.muted = number % 2 == 1;
    r.deemphasisIndex = number % 3;
    r.stereoEnabled = number % 2 == 0;
    r.nrEnabled = number % 2 == 1;
    r.nrStrength = 0.1 * number;
    r.agcMode = number % 5;
    r.agcFixedGainDb = -20.0 + 5.0 * number;
    r.nbEnabled = number % 2 == 0;
    r.nbThresholdDb = 6.0 + number;
    r.notchEnabled = number % 3 == 1;
    r.notchFreqHz = 500.0 * number;
    r.notchQ = 10.0 + number;
    r.autoNotch = number % 4 == 2;
    r.audioRoute = number % 4;
    r.running = number % 2 == 0;
    return r;
}

AppConfig withRadios(int count) {
    AppConfig c;
    c.centerHz = 101.1e6;
    c.mode = "WFM";
    c.volume = 0.7f;
    c.agcMode = 2;
    c.nbEnabled = true;
    for (int n = 2; n < 2 + count; ++n) { c.radios.push_back(fullRadio(n)); }
    return c;
}

// The text of a file shaped like the one 0.99.104 wrote: radio 1's keys, the plugin keys, and keys the reader does not know (as a newer file would have).
const char* kOldFile = R"({
  "schemaVersion": 1,
  "sourceKind": "rtlsdr",
  "soapyArgs": "",
  "nativeArgs": "serial=00000001",
  "centerHz": 98500000.0,
  "mode": "WFM",
  "bandwidthHz": 150000.0,
  "squelchDb": -55.0,
  "volume": 0.35,
  "sampleRateHz": 2400000.0,
  "stereoEnabled": true,
  "deemphasisIndex": 1,
  "nrEnabled": true,
  "nrStrength": 0.8,
  "notchEnabled": false,
  "vfoOffsetHz": 120000.0,
  "pluginRun": {"pocsag-decoder": "always"},
  "futureKeyThatNoBuildKnows": [1, 2, 3]
})";

void sameRadio(const SavedRadio& a, const SavedRadio& b) {
    CHECK_EQ(a.number, b.number);
    CHECK_EQ(a.sourceKind, b.sourceKind);
    CHECK_EQ(a.args, b.args);
    CHECK_EQ(a.sampleRateHz, b.sampleRateHz);
    CHECK_EQ(a.antenna, b.antenna);
    CHECK_EQ(a.toneHz, b.toneHz);
    CHECK_EQ(a.centerHz, b.centerHz);
    CHECK_EQ(a.mode, b.mode);
    CHECK_EQ(a.bandwidthHz, b.bandwidthHz);
    CHECK_EQ(a.vfoOffsetHz, b.vfoOffsetHz);
    CHECK_EQ(a.squelchDb, b.squelchDb);
    CHECK_EQ(a.squelchAuto, b.squelchAuto);
    CHECK(std::fabs(a.volume - b.volume) < 1e-12);
    CHECK_EQ(a.muted, b.muted);
    CHECK_EQ(a.deemphasisIndex, b.deemphasisIndex);
    CHECK_EQ(a.stereoEnabled, b.stereoEnabled);
    CHECK_EQ(a.nrEnabled, b.nrEnabled);
    CHECK(std::fabs(a.nrStrength - b.nrStrength) < 1e-12);
    CHECK_EQ(a.agcMode, b.agcMode);
    CHECK_EQ(a.agcFixedGainDb, b.agcFixedGainDb);
    CHECK_EQ(a.nbEnabled, b.nbEnabled);
    CHECK_EQ(a.nbThresholdDb, b.nbThresholdDb);
    CHECK_EQ(a.notchEnabled, b.notchEnabled);
    CHECK_EQ(a.notchFreqHz, b.notchFreqHz);
    CHECK_EQ(a.notchQ, b.notchQ);
    CHECK_EQ(a.autoNotch, b.autoNotch);
    CHECK_EQ(a.audioRoute, b.audioRoute);
    CHECK_EQ(a.running, b.running);
}

bool parseText(const std::string& text, AppConfig& out) {
    std::string error;
    return ConfigStore::parse(text, "test", out, error);
}

// ---- A -------------------------------------------------------------------------------------------------------------------------------------------

void testRoundTrip() {
    for (const int count : {0, 1, 7}) {
        const AppConfig in = withRadios(count);
        const std::string text = ConfigStore::serialize(in);
        const json doc = json::parse(text);
        CHECK_EQ(doc.contains("radios"), count > 0);
        AppConfig out;
        CHECK(parseText(text, out));
        CHECK_EQ(out.radios.size(), static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) { sameRadio(in.radios[static_cast<std::size_t>(i)], out.radios[static_cast<std::size_t>(i)]); }
        // radio 1 is the top-level keys and is untouched by the array
        CHECK_EQ(out.centerHz, in.centerHz);
        CHECK_EQ(out.mode, in.mode);
        CHECK_EQ(out.agcMode, 2);
        CHECK(out.nbEnabled);
        // and a second pass over the result is the same text (nothing drifts on a round trip)
        CHECK_EQ(ConfigStore::serialize(out), text);
    }
    // eight radios in all = radio 1 plus seven entries, numbered 2..8 in order
    const AppConfig eight = withRadios(7);
    CHECK_EQ(eight.radios.front().number, 2);
    CHECK_EQ(eight.radios.back().number, 8);
}

// ---- B -------------------------------------------------------------------------------------------------------------------------------------------

void testTheOldFileLoadsUnchanged() {
    AppConfig c;
    CHECK(parseText(kOldFile, c));
    CHECK(c.radios.empty());
    CHECK(c.pluginRadio.empty());
    CHECK_EQ(c.sourceKind, std::string("rtlsdr"));
    CHECK_EQ(c.nativeArgs, std::string("serial=00000001"));
    CHECK_EQ(c.centerHz, 98500000.0);
    CHECK_EQ(c.mode, std::string("WFM"));
    CHECK_EQ(c.squelchDb, -55.0f);
    CHECK(std::fabs(c.volume - 0.35f) < 1e-6);
    CHECK_EQ(c.deemphasisIndex, 1);
    CHECK(c.nrEnabled);
    CHECK_EQ(c.vfoOffsetHz, 120000.0);
    CHECK_EQ(c.pluginRun.count("pocsag-decoder"), static_cast<std::size_t>(1));
    // written back, it carries neither new key: a one-radio user's file gains nothing
    const json back = json::parse(ConfigStore::serialize(c));
    CHECK(!back.contains("radios"));
    CHECK(!back.contains("pluginRadio"));
    CHECK_EQ(back["schemaVersion"].get<int>(), 1);
    CHECK_EQ(back["nativeArgs"].get<std::string>(), std::string("serial=00000001"));
    // a file with no schemaVersion at all (the oldest files) still loads
    AppConfig noVersion;
    CHECK(parseText(R"({"centerHz": 7100000.0})", noVersion));
    CHECK(noVersion.radios.empty());
    CHECK_EQ(noVersion.centerHz, 7100000.0);
}

// ---- C -------------------------------------------------------------------------------------------------------------------------------------------

void testTheNewFileAsAnOldBuildReadsIt() {
    AppConfig many = withRadios(7);
    many.pluginRadio["pocsag-decoder"] = 3;
    many.pluginRadio["ads-b"] = 8;
    AppConfig oneRadio = many;
    oneRadio.radios.clear();
    oneRadio.pluginRadio.clear();
    json withNew = json::parse(ConfigStore::serialize(many));
    const json oldShape = json::parse(ConfigStore::serialize(oneRadio));
    CHECK_EQ(withNew["schemaVersion"].get<int>(), 1);   // an old build rejects any other value outright
    CHECK(withNew.contains("radios"));
    CHECK(withNew.contains("pluginRadio"));
    // The set of keys an old build reads is untouched: the new file has these two keys more and nothing else, and every other key has the value it had.
    std::set<std::string> added;
    for (auto it = withNew.begin(); it != withNew.end(); ++it) {
        if (!oldShape.contains(it.key())) { added.insert(it.key()); }
    }
    CHECK(added == (std::set<std::string>{"radios", "pluginRadio"}));
    withNew.erase("radios");
    withNew.erase("pluginRadio");
    CHECK(withNew == oldShape);
    // An old reader is a reader that ignores keys it does not know: this tree's reader, handed the new file with the unknown keys removed, reads radio 1 exactly as it reads
    // the full file.
    AppConfig full;
    AppConfig stripped;
    CHECK(parseText(ConfigStore::serialize(many), full));
    CHECK(parseText(withNew.dump(), stripped));
    CHECK_EQ(full.centerHz, stripped.centerHz);
    CHECK_EQ(full.mode, stripped.mode);
    CHECK_EQ(full.agcMode, stripped.agcMode);
    CHECK_EQ(full.sourceKind, stripped.sourceKind);
    CHECK_EQ(full.nativeArgs, stripped.nativeArgs);
    CHECK(stripped.radios.empty());
    // and an old build's write-back (the stripped config) names no radio 2..8: radio 1 survives, the rest are forgotten
    const json oldWrites = json::parse(ConfigStore::serialize(stripped));
    CHECK(!oldWrites.contains("radios"));
    CHECK_EQ(oldWrites["centerHz"].get<double>(), many.centerHz);
}

// ---- D -------------------------------------------------------------------------------------------------------------------------------------------

void checkWhole(const SavedRadio& r) {
    CHECK(r.number >= 2 && r.number <= 8);
    CHECK(r.sampleRateHz >= 0.0);
    CHECK(r.centerHz >= 0.0);
    CHECK(r.bandwidthHz >= 0.0);
    CHECK(r.squelchDb >= -200.0 && r.squelchDb <= 20.0);
    CHECK(r.volume >= 0.0 && r.volume <= 1.0);
    CHECK(r.deemphasisIndex >= 0 && r.deemphasisIndex <= 2);
    CHECK(r.nrStrength >= 0.0 && r.nrStrength <= 1.0);
    CHECK(r.agcMode >= 0 && r.agcMode <= 4);
    CHECK(r.agcFixedGainDb >= -20.0 && r.agcFixedGainDb <= 40.0);
    CHECK(r.nbThresholdDb >= 6.0 && r.nbThresholdDb <= 30.0);
    CHECK(r.notchFreqHz >= 10.0 && r.notchFreqHz <= 20000.0);
    CHECK(r.notchQ >= 0.1 && r.notchQ <= 1000.0);
    CHECK(r.audioRoute >= 0 && r.audioRoute <= 3);
    CHECK(r.args.size() <= 512u);
}

std::string damaged(const std::string& radiosJson) {
    return R"({"schemaVersion": 1, "centerHz": 145800000.0, "mode": "NFM", "volume": 0.25, "agcMode": 3, "radios": )" + radiosJson + "}";
}

void testADamagedArray() {
    // The wrong type at the key, and entries that are not objects: radio 1 is whole, there are no extra radios.
    for (const char* bad : {"\"nope\"", "42", "true", "null", "{\"a\": 1}", "[1, \"two\", null, [3], true]", "[]"}) {
        AppConfig c;
        CHECK(parseText(damaged(bad), c));
        CHECK(c.radios.empty());
        CHECK_EQ(c.centerHz, 145800000.0);
        CHECK_EQ(c.mode, std::string("NFM"));
        CHECK_EQ(c.agcMode, 3);
    }
    // Fields of the wrong type keep their defaults; the entry survives.
    {
        AppConfig c;
        CHECK(parseText(damaged(R"([{"number": "three", "sourceKind": 7, "centerHz": "loud", "volume": [1], "muted": "yes", "agcMode": 2.5, "running": 1}])"), c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(1));
        const SavedRadio& r = c.radios[0];
        CHECK_EQ(r.number, 2);   // no usable number: the lowest free one
        CHECK_EQ(r.sourceKind, std::string("siggen"));
        CHECK_EQ(r.centerHz, 0.0);
        CHECK_EQ(r.volume, 0.5);
        CHECK(!r.muted);
        CHECK_EQ(r.agcMode, 0);
        CHECK(!r.running);
        CHECK_EQ(c.centerHz, 145800000.0);
    }
    // NINE entries: at most seven survive (radios 2..8), the first seven, with their own numbers.
    {
        std::string nine = "[";
        for (int i = 0; i < 9; ++i) { nine += std::string(i ? "," : "") + "{\"number\": " + std::to_string(2 + i) + ", \"centerHz\": " + std::to_string(1000000 * (i + 1)) + "}"; }
        nine += "]";
        AppConfig c;
        CHECK(parseText(damaged(nine), c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(7));
        for (std::size_t i = 0; i < c.radios.size(); ++i) {
            CHECK_EQ(c.radios[i].number, static_cast<int>(i) + 2);
            CHECK_EQ(c.radios[i].centerHz, 1000000.0 * static_cast<double>(i + 1));
        }
        CHECK_EQ(c.centerHz, 145800000.0);
    }
    // Four hundred entries cannot make the reader work for long or keep more than seven.
    {
        std::string many = "[";
        for (int i = 0; i < 400; ++i) { many += std::string(i ? "," : "") + "{}"; }
        many += "]";
        AppConfig c;
        CHECK(parseText(damaged(many), c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(7));
    }
    // Repeated and impossible numbers: every survivor has a distinct number from 2 to 8; the file's order is kept.
    {
        AppConfig c;
        CHECK(parseText(damaged(R"([{"number": 3}, {"number": 3}, {"number": 1}, {"number": 9}, {"number": -4}, {"number": 0}, {"number": 8}])"), c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(7));
        std::set<int> numbers;
        for (const SavedRadio& r : c.radios) { numbers.insert(r.number); }
        CHECK_EQ(numbers.size(), static_cast<std::size_t>(7));
        CHECK(*numbers.begin() == 2 && *numbers.rbegin() == 8);
        CHECK_EQ(c.radios[0].number, 3);   // the first to claim a number keeps it
        CHECK_EQ(c.radios[6].number, 8);
    }
    // Negative and absurd numbers: every value is inside its range.
    {
        AppConfig c;
        CHECK(parseText(damaged(R"([{"sampleRateHz": -2400000, "centerHz": -98e6, "bandwidthHz": -150000, "toneHz": 1e30, "vfoOffsetHz": -1e300, "squelchDb": -9999,
            "volume": -3, "deemphasisIndex": -5, "nrStrength": 40, "agcMode": -1, "agcFixedGainDb": -500, "nbThresholdDb": 1e9, "notchFreqHz": -1, "notchQ": 0,
            "audioRoute": 99}, {"volume": 7, "deemphasisIndex": 12, "agcMode": 900, "agcFixedGainDb": 500, "nbThresholdDb": -1e9, "notchFreqHz": 1e9, "notchQ": 1e9,
            "audioRoute": -2, "squelchDb": 500, "centerHz": 1e300}])"),
                          c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(2));
        for (const SavedRadio& r : c.radios) { checkWhole(r); }
        CHECK_EQ(c.radios[0].sampleRateHz, 0.0);
        CHECK_EQ(c.radios[0].centerHz, 0.0);
        CHECK_EQ(c.radios[0].bandwidthHz, 0.0);
        CHECK_EQ(c.radios[0].toneHz, 0.0);
        CHECK_EQ(c.radios[0].vfoOffsetHz, 0.0);
        CHECK_EQ(c.radios[0].squelchDb, -200.0);
        CHECK_EQ(c.radios[0].volume, 0.0);
        CHECK_EQ(c.radios[0].audioRoute, 3);
        CHECK_EQ(c.radios[1].volume, 1.0);
        CHECK_EQ(c.radios[1].squelchDb, 20.0);
        CHECK_EQ(c.radios[1].agcMode, 4);
        CHECK_EQ(c.radios[1].centerHz, 0.0);
        CHECK_EQ(c.centerHz, 145800000.0);
    }
    // A sound card cannot be a second radio (its one list and open are the first radio's); a file, a Pluto and an rtl_tcp server can (stage 7: they are kept with their arguments); an unknown
    // driver is the generator; device arguments of a megabyte are dropped.
    {
        std::string huge(1 << 20, 'x');
        AppConfig c;
        CHECK(parseText(damaged(R"([{"sourceKind": "file", "args": "C:/x.iq"}, {"sourceKind": "soundcard", "args": "card"}, {"sourceKind": "pluto", "args": "uri=ip:1"},
            {"sourceKind": "rtltcp", "args": "rtltcp=1.2.3.4:1234"}, {"sourceKind": "rtlsdr", "args": ")" + huge + R"("}, {"sourceKind": "no such driver", "args": "a"},
            {"sourceKind": "sdrplay", "args": "serial=1"}])"),
                          c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(7));
        CHECK_EQ(c.radios[0].sourceKind, std::string("file"));
        CHECK_EQ(c.radios[0].args, std::string("C:/x.iq"));
        CHECK_EQ(c.radios[2].sourceKind, std::string("pluto"));
        CHECK_EQ(c.radios[2].args, std::string("uri=ip:1"));
        CHECK_EQ(c.radios[3].sourceKind, std::string("rtltcp"));
        CHECK_EQ(c.radios[3].args, std::string("rtltcp=1.2.3.4:1234"));
        for (const std::size_t i : {1u, 4u, 5u}) {
            CHECK_EQ(c.radios[i].sourceKind, std::string("siggen"));
            CHECK(c.radios[i].args.empty());
        }
        CHECK_EQ(c.radios[6].sourceKind, std::string("sdrplay"));
        CHECK_EQ(c.radios[6].args, std::string("serial=1"));
    }
    // Duplicate DEVICES are not the loader's to refuse (the engine's ledger does, by name): both entries are kept.
    {
        AppConfig c;
        CHECK(parseText(damaged(R"([{"sourceKind": "rtlsdr", "args": "serial=1"}, {"sourceKind": "rtlsdr", "args": "serial=1"}])"), c));
        CHECK_EQ(c.radios.size(), static_cast<std::size_t>(2));
        CHECK_EQ(c.radios[0].args, c.radios[1].args);
    }
    // And a damaged array does not stop the rest of the file from loading: the top-level keys after it are read.
    {
        AppConfig c;
        CHECK(parseText(R"({"radios": "broken", "centerHz": 7074000.0, "pluginRun": {"a": "stopped"}})", c));
        CHECK_EQ(c.centerHz, 7074000.0);
        CHECK_EQ(c.pluginRun.count("a"), static_cast<std::size_t>(1));
    }
}

// ---- E -------------------------------------------------------------------------------------------------------------------------------------------

void testThePluginsRadio() {
    AppConfig c;
    CHECK(parseText(R"({"pluginRadio": {"pocsag-decoder": 3, "": 4, "ads-b": 1, "ais": 9, "acars": "2", "wefax": 2.5, "rtty": 8, "x": -3}})", c));
    CHECK_EQ(c.pluginRadio.size(), static_cast<std::size_t>(3));
    CHECK_EQ(c.pluginRadio.at("pocsag-decoder"), 3);
    CHECK_EQ(c.pluginRadio.at("ads-b"), 1);   // (a 1 is legal in a file; the engine itself never writes one)
    CHECK_EQ(c.pluginRadio.at("rtty"), 8);
    for (const char* bad : {"\"s\"", "[1]", "7", "null"}) {
        AppConfig d;
        CHECK(parseText(std::string("{\"pluginRadio\": ") + bad + ", \"centerHz\": 5e6}", d));
        CHECK(d.pluginRadio.empty());
        CHECK_EQ(d.centerHz, 5e6);
    }
    // Written only when there is an entry; an empty map writes no key.
    AppConfig w;
    w.pluginRadio["b"] = 2;
    w.pluginRadio["c"] = 99;   // not a radio: dropped on the way out as on the way in
    const json doc = json::parse(ConfigStore::serialize(w));
    CHECK(doc.contains("pluginRadio"));
    CHECK(doc["pluginRadio"].contains("b"));
    CHECK(!doc["pluginRadio"].contains("c"));
    AppConfig none;
    CHECK(!json::parse(ConfigStore::serialize(none)).contains("pluginRadio"));
    none.pluginRadio["c"] = 99;
    CHECK(!json::parse(ConfigStore::serialize(none)).contains("pluginRadio"));
}

}  // namespace

int main() {
    std::printf("test_engine_api_config_radios\n");
    testRoundTrip();
    testTheOldFileLoadsUnchanged();
    testTheNewFileAsAnOldBuildReadsIt();
    testADamagedArray();
    testThePluginsRadio();
    return check::finish("test_engine_api_config_radios");
}
