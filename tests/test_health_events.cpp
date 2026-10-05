// The anonymous count of failures that are NOT crashes (core/health_events.hpp,
// 0.99.64): the vocabulary, its encoding, the ledger that carries it across a
// crash, and the promises written about it in PRIVACY.md and the Worker.
//
// WHAT THIS HOLDS, and the shape of each promise:
//
//   - THE VOCABULARY IS CLOSED. Every token the builders can make is legal, every
//     legal token is made from enums or reduced words, and a word that is not in
//     the tables - a device name, a serial, a path, an error message - cannot be
//     made into a token, written to the ledger, read back from its file or
//     encoded for the record.
//   - THE ENCODING IS STRICT AND BOUNDED: decode() refuses anything encode()
//     would not write; the worst case fits the cap the Worker enforces.
//   - THE LEDGER IS stall-ledger-shaped: a count is on disk before the session
//     could save anything, survives a process that never ran another line, is
//     taken off only by an accepted record, belongs to the right session, counts
//     nothing with reporting off or Diagnostics off, and is removed when
//     reporting is switched off.
//   - THE REASONS ARE READ OFF WHAT THE DRIVERS REALLY SAY: the sentences are the
//     drivers' own (the shared constants, the SDRplay functions that return the
//     text, a real SoapySDR failure), in English and - for the few that go
//     through tr() - in German.
//   - THE DOCUMENT AND THE WORKER SAY THE SAME THING AS THE CODE: PRIVACY.md lists
//     every word of the vocabulary, one table per list, and telemetry-worker/
//     worker.js carries the same lists; a word added in one place and not the
//     others fails here, in both directions.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health_events.hpp"
#include "core/i18n.hpp"
#include "core/telemetry.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"
#include "usb/usb_device.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace health = cascade::core::health;
using cascade::core::StallLedger;
using cascade::core::TelemetryReporter;
using health::Counts;
using health::HealthLedger;

namespace {

fs::path scratch(const char* tag) {
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::temp_directory_path();
    const fs::path d = base / (std::string("cascade-health-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

bool waitGone(const fs::path& p) {
    for (int i = 0; i < 400; ++i) {
        if (!fs::exists(p)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !fs::exists(p);
}

std::string sourceFile(const char* relative) {
    return readFile(fs::path(CASCADE_SOURCE_DIR) / relative);
}

const std::string kBusy = "radio_fail.rtlsdr.busy";
// A count by token, 0 when it is not there - a missing token is a failed check,
// never an exception that ends the run (and hides every check after it).
std::uint32_t countOf(const Counts& c, const std::string& token) {
    const auto it = c.find(token);
    return it == c.end() ? 0u : it->second;
}


// ---------------------------------------------------------------------------
// The vocabulary is closed.
// ---------------------------------------------------------------------------
void testEveryTokenIsLegalAndNothingElseIs() {
    const std::vector<std::string> all = health::allTokens();
    // 1 scan + 12 + 12 + 96 (driver x reason) + 11 + 55 (api x reason) + 4 update
    // + 1 catalogue + 4 + 3 plugin + 1 recording
    CHECK(all.size() == 200);
    std::set<std::string> unique(all.begin(), all.end());
    CHECK(unique.size() == all.size());
    for (const std::string& t : all) {
        if (!health::validToken(t)) { std::printf("FAIL %s is in allTokens() and not valid\n", t.c_str()); }
        CHECK(health::validToken(t));
        CHECK(t.size() <= 29);
        for (char c : t) { CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'); }
    }

    // What is NOT a token: case, a missing or extra qualifier, a stray byte, free text.
    const std::vector<std::string> bad = {
        "", ".", "scan_none.", "scan_none.x", "radio_fail", "radio_fail.rtlsdr",
        "radio_fail.rtlsdr.busy.extra", "radio_fail.RTLSDR.busy", "Radio_fail.rtlsdr.busy",
        "radio_fail.rtlsdr.nope", "radio_fail..busy", "radio_fail.rtlsdr.busy ", " scan_none",
        "scan_none=1", "scan_none,scan_none", "sound_ok.WASAPI", "sound_ok", "plug_inst.net.net",
        "plug_load.net", "radio_open.my-radio", "radio_open.C:\\Users\\someone\\x", "rec_fail\n",
        std::string("scan_none\0", 10), std::string(60, 'a'), "upd_dl.", "upd_check.x"};
    for (const std::string& t : bad) {
        if (health::validToken(t)) { std::printf("FAIL %s should not be a token\n", t.c_str()); }
        CHECK(!health::validToken(t));
    }
}

void testBuildersCannotCarryFreeText() {
    // Every enum value of every builder is a legal token.
    using health::RadioReason;
    for (int r = 0; r <= static_cast<int>(RadioReason::Other); ++r) {
        for (const char* d : {"rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr", "rx888",
                              "pluto", "aor", "soapy", "soundcard"}) {
            CHECK(health::validToken(health::tokenRadioFail(d, static_cast<RadioReason>(r))));
        }
    }
    for (int a = 0; a <= static_cast<int>(health::AudioApi::Other); ++a) {
        CHECK(health::validToken(health::tokenSoundOk(static_cast<health::AudioApi>(a))));
        for (int s = 0; s <= static_cast<int>(health::SoundReason::Other); ++s) {
            CHECK(health::validToken(health::tokenSoundFail(static_cast<health::AudioApi>(a),
                                                            static_cast<health::SoundReason>(s))));
        }
    }
    for (int c = 0; c <= static_cast<int>(health::InstallClass::Other); ++c) {
        CHECK(health::validToken(health::tokenPluginInstall(static_cast<health::InstallClass>(c))));
    }
    for (int c = 0; c <= static_cast<int>(health::LoadClass::Load); ++c) {
        CHECK(health::validToken(health::tokenPluginLoad(static_cast<health::LoadClass>(c))));
    }

    // A DRIVER NAME IS REDUCED, never passed on: a model, a serial, a path, a
    // sentence, an empty string, a name with a capital - all are "other".
    for (const char* d : {"RTL2838UHIDIR", "serial=00000001", "C:\\Users\\someone\\radio", "",
                          "rtlsdr, serial=0001", "My Radio", "RTLSDR", "soapy/uhd", "rtlsdr\n"}) {
        const std::string t = health::tokenRadioFail(d, RadioReason::Busy);
        if (d == std::string("RTLSDR")) {
            CHECK(t == "radio_fail.rtlsdr.busy");   // case is folded: it IS the word
        } else {
            CHECK(t == "radio_fail.other.busy");
        }
        CHECK(health::validToken(t));
    }
    CHECK(health::driverWord("rtlsdr") == "rtlsdr");
    // ...and a hostile spray of random text can only ever come out as a word of the table.
    std::mt19937 rng(7);
    std::set<std::string> words;
    for (const std::string& t : health::allTokens()) {
        if (t.rfind("radio_open.", 0) == 0) { words.insert(t.substr(11)); }
    }
    for (int i = 0; i < 2000; ++i) {
        std::string junk;
        const int n = static_cast<int>(rng() % 20);
        for (int k = 0; k < n; ++k) { junk += static_cast<char>(rng() % 256); }
        CHECK(words.count(health::driverWord(junk)) == 1);
    }
    // The host API likewise.
    CHECK(health::audioApiFromName("Windows WASAPI") == health::AudioApi::Wasapi);
    CHECK(health::audioApiFromName("MME") == health::AudioApi::Mme);
    CHECK(health::audioApiFromName("Windows DirectSound") == health::AudioApi::DirectSound);
    CHECK(health::audioApiFromName("Windows WDM-KS") == health::AudioApi::Wdmks);
    CHECK(health::audioApiFromName("ASIO") == health::AudioApi::Asio);
    CHECK(health::audioApiFromName("ALSA") == health::AudioApi::Alsa);
    CHECK(health::audioApiFromName("JACK Audio Connection Kit") == health::AudioApi::Jack);
    CHECK(health::audioApiFromName("OSS") == health::AudioApi::Oss);
    CHECK(health::audioApiFromName("Core Audio") == health::AudioApi::CoreAudio);
    CHECK(health::audioApiFromName("") == health::AudioApi::None);
    CHECK(health::audioApiFromName("Alice's AirPods Pro") == health::AudioApi::Other);
}

// ---------------------------------------------------------------------------
// The encoding is strict and bounded.
// ---------------------------------------------------------------------------
void testEncodingIsStrictAndBounded() {
    Counts c = {{"sound_ok.wasapi", 1}, {kBusy, 2}, {"scan_none", 1}, {"radio_open.rtlsdr", 3}};
    // The written order is the vocabulary's, whatever order the map holds.
    CHECK(health::encode(c) == "scan_none=1,radio_open.rtlsdr=3,radio_fail.rtlsdr.busy=2,sound_ok.wasapi=1");
    CHECK(health::encode(Counts()).empty());

    Counts back;
    CHECK(health::decode(health::encode(c), back));
    CHECK(back == c);
    CHECK(health::decode("", back) && back.empty());

    // encode leaves out what is not legal: an unknown token, a zero count.
    Counts dirty = {{"scan_none", 1}, {"not_a_token", 5}, {"rec_fail", 0}, {"serial=0001", 1}};
    CHECK(health::encode(dirty) == "scan_none=1");
    // ...and clamps a count to 999.
    CHECK(health::encode({{"scan_none", 123456}}) == "scan_none=999");

    // decode refuses everything encode would not write.
    const std::vector<std::string> bad = {
        "scan_none", "scan_none=", "scan_none=0", "scan_none=1000", "scan_none=-1", "scan_none=1.5",
        "scan_none=01x", "scan_none=1,", ",scan_none=1", "scan_none=1,,rec_fail=1", " scan_none=1",
        "scan_none=1,scan_none=1", "nope=1", "radio_fail.rtlsdr=1", "SCAN_NONE=1", "scan_none=1;rec_fail=1",
        "scan_none=١", std::string("scan_none=1\0", 12), std::string(900, 'a')};
    for (const std::string& t : bad) {
        Counts out = {{"stale", 1}};
        if (health::decode(t, out)) { std::printf("FAIL decode accepted \"%s\"\n", t.c_str()); }
        CHECK(!health::decode(t, out));
        CHECK(out.empty());
    }

    // sanitise keeps what is legal and drops the rest, in order, merged, clamped.
    CHECK(health::sanitise("rec_fail=2,junk=1,scan_none=1,rec_fail=3,upd_dl=5000,x") ==
          "scan_none=1,upd_dl=999,rec_fail=5");
    CHECK(health::sanitise("serial=00000001,C:\\Users\\someone,a message with spaces").empty());
    CHECK(health::sanitise(std::string(100000, 'x')).empty());

    // THE WORST CASE FITS the cap the Worker enforces: the 8 longest radio
    // failures and the 16 longest of everything else, every count at 999.
    std::vector<std::string> fails, others;
    for (const std::string& t : health::allTokens()) {
        (t.rfind("radio_fail.", 0) == 0 ? fails : others).push_back(t);
    }
    auto longest = [](std::vector<std::string>& v, std::size_t n) {
        std::stable_sort(v.begin(), v.end(),
                         [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        v.resize(n);
    };
    longest(fails, health::kMaxRadioFailTokens);
    longest(others, health::kMaxTokens - health::kMaxRadioFailTokens);
    Counts worst;
    for (const auto& t : fails) { worst[t] = 999; }
    for (const auto& t : others) { worst[t] = 999; }
    CHECK(worst.size() == health::kMaxTokens);
    const std::string widest = health::encode(worst);
    CHECK(health::decode(widest, back) && back == worst);   // none was cut for length
    CHECK(widest.size() <= health::kMaxEncodedBytes);
    std::printf("health: the widest legal record is %zu bytes (cap %zu)\n", widest.size(),
                health::kMaxEncodedBytes);
}

// ---------------------------------------------------------------------------
// The ledger.
// ---------------------------------------------------------------------------
void testCountsOnceAndOftenAsDeclared() {
    const fs::path dir = scratch("counting");
    const std::string id = cascade::core::newInstallId();
    HealthLedger l;
    l.arm(HealthLedger::pathIn(u8(dir)), id, false);
    CHECK(l.armed() && l.decided());

    l.note(kBusy);
    l.note(kBusy);
    l.note(health::tokenRadioOpen("rtlsdr"));
    // ONCE A SESSION: a watchdog retrying the output, a rescan meeting the same
    // refused plugin, a Refresh that finds nothing again.
    for (int i = 0; i < 5; ++i) {
        l.note(health::tokenScanNone());
        l.note(health::tokenSoundOk(health::AudioApi::Wasapi));
        l.note(health::tokenSoundFail(health::AudioApi::Mme, health::SoundReason::Busy));
        l.note(health::tokenPluginLoad(health::LoadClass::Abi));
    }
    // ...but a different token of the same event is its own fact.
    l.note(health::tokenSoundFail(health::AudioApi::Mme, health::SoundReason::NoDevice));
    // A token that is not in the vocabulary is nothing.
    l.note("serial=00000001");
    l.note("radio_fail.rtlsdr.busy extra");
    l.note("");

    const Counts c = l.counts();
    CHECK(countOf(c, kBusy) == 2);
    CHECK(countOf(c, "radio_open.rtlsdr") == 1);
    CHECK(countOf(c, "scan_none") == 1);
    CHECK(countOf(c, "sound_ok.wasapi") == 1);
    CHECK(countOf(c, "sound_fail.mme.busy") == 1);
    CHECK(countOf(c, "sound_fail.mme.nodevice") == 1);
    CHECK(countOf(c, "plug_load.abi") == 1);
    CHECK(c.size() == 7);
    CHECK(l.encoded() == health::encode(c));
}

void testBoundsHold() {
    HealthLedger l;
    l.arm("", cascade::core::newInstallId(), false);
    // Far more distinct tokens than may be carried: the first 24 are kept.
    std::size_t sent = 0;
    for (const std::string& t : health::allTokens()) {
        if (t.rfind("radio_fail.", 0) == 0) { continue; }
        l.note(t);
        ++sent;
    }
    CHECK(sent > health::kMaxTokens);
    CHECK(l.counts().size() == health::kMaxTokens);

    // Radio failures are the one wide family: at most 8 distinct pairs, even in an
    // otherwise empty ledger.
    HealthLedger r;
    r.arm("", cascade::core::newInstallId(), false);
    for (const std::string& t : health::allTokens()) {
        if (t.rfind("radio_fail.", 0) == 0) { r.note(t); }
    }
    CHECK(r.counts().size() == health::kMaxRadioFailTokens);
    // A pair already carried still counts.
    const std::string first = r.counts().begin()->first;
    r.note(first);
    CHECK(countOf(r.counts(), first) == 2);

    // A count stops at 999.
    HealthLedger big;
    big.arm("", cascade::core::newInstallId(), false);
    for (int i = 0; i < 1500; ++i) { big.note(kBusy); }
    CHECK(countOf(big.counts(), kBusy) == health::kMaxCount);
}

void testACountSurvivesACrash() {
    const fs::path dir = scratch("crash");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));
    {
        HealthLedger a;
        a.arm(file, id, false);
        // From a thread that is not this one - a driver's worker, the updater's.
        std::thread worker([&a] {
            a.note(kBusy);
            a.note(kBusy);
            a.note(health::tokenUpdateDownload());
        });
        worker.join();
        CHECK(a.flush());
        // ON DISK BEFORE ANYTHING COULD SAVE A CONFIG: the process is ended here.
        CHECK(readFile(file) == HealthLedger::fileText(id, a.counts()));
    }
    HealthLedger next;
    next.arm(file, id, /*loadExisting=*/true);
    CHECK(countOf(next.priorCounts(), kBusy) == 2);
    CHECK(countOf(next.priorCounts(), "upd_dl") == 1);
    CHECK(next.counts() == next.priorCounts());

    // A copy left by an EARLIER IDENTITY is not attributed to this one.
    HealthLedger other;
    other.arm(file, cascade::core::newInstallId(), true);
    CHECK(other.counts().empty());

    // Whatever else is in the file is no count: a wrong first line, no first line.
    const std::vector<std::string> junk = {
        "", "garbage", "scan_none=1\n", id.substr(0, 31) + "\nscan_none=1\n",
        "someone@example.com\nscan_none=1\n", std::string("\xff\xfe\0", 3)};
    for (const std::string& j : junk) {
        writeFile(file, j);
        HealthLedger s;
        s.arm(file, id, true);
        CHECK(s.counts().empty());
    }
    // Lines that are not a legal count are skipped, the others kept, bounds applied.
    writeFile(file, id + "\r\nscan_none=1\r\nnope=3\r\nrec_fail=0\r\nupd_dl=99999\r\nupd_run=x\r\nplug_cat=2\r\n");
    HealthLedger some;
    some.arm(file, id, true);
    CHECK(some.counts() == (Counts{{"scan_none", 1}, {"upd_dl", 999}, {"plug_cat", 2}}));
    // A hand-edited file with 100 radio failures keeps 8 of them.
    std::string many = id + "\n";
    int n = 0;
    for (const std::string& t : health::allTokens()) {
        if (t.rfind("radio_fail.", 0) == 0 && n++ < 100) { many += t + "=1\n"; }
    }
    writeFile(file, many);
    HealthLedger bounded;
    bounded.arm(file, id, true);
    CHECK(bounded.counts().size() == health::kMaxRadioFailTokens);
    // A file that is simply missing is nothing.
    fs::remove(file);
    HealthLedger none;
    none.arm(file, id, true);
    CHECK(none.counts().empty());
}

void testTheRecordCarriesEarlierSessionsAndNotThisOne() {
    // THE WRONG-VERSION TRAP. The record sent at start-up describes the PREVIOUS
    // session; what this run has counted since (a saved radio that would not
    // open, which is the first thing it does) belongs to THIS session and must
    // not be attached to that record.
    const fs::path dir = scratch("session");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));
    writeFile(file, HealthLedger::fileText(id, Counts{{kBusy, 2}, {"upd_check", 1}}));

    HealthLedger l;
    // Before the application has read its config: held in memory, decided later.
    l.note(health::tokenRadioFail("rtlsdr", health::RadioReason::Bind));
    CHECK(!l.decided() && !l.armed());
    CHECK(l.encoded().empty());          // nothing is "kept" for a record yet
    l.arm(file, id, /*loadExisting=*/true);
    CHECK(l.priorCounts() == (Counts{{kBusy, 2}, {"upd_check", 1}}));
    CHECK(countOf(l.counts(), "radio_fail.rtlsdr.bind") == 1);   // this session's, held since before the decision
    l.note(health::tokenScanNone());                        // and after

    cascade::core::TelemetryReport r;
    r.installId = id;
    r.appVersion = "0.99.64";
    r.launches = 3;
    r.session.seconds = 60;
    const std::string stored = r.toJson();
    StallLedger stalls;
    stalls.arm("", id, false);

    std::string out;
    std::uint64_t carriedStalls = 9;
    Counts carried;
    CHECK(cascade::core::prepareStartupRecord(u8(dir), stored, stalls, l, out, carriedStalls, carried));
    // EARLIER SESSIONS ONLY.
    CHECK(carried == (Counts{{kBusy, 2}, {"upd_check", 1}}));
    const nlohmann::json j = nlohmann::json::parse(out);
    CHECK(j["health"] == "radio_fail.rtlsdr.busy=2,upd_check=1");
    CHECK(j["v"] == "0.99.64");
    CHECK(j["stalls"] == 0);

    // Accepted: exactly what was carried is taken off; this session's stays,
    // on disk, for the NEXT record.
    {
        TelemetryReporter rep;
        rep.sendVia([](const std::string&, const std::string&) { return true; }, "https://x/", out,
                    cascade::core::settleOnAccept(std::make_shared<StallLedger>(), carriedStalls,
                                                  std::shared_ptr<HealthLedger>(&l, [](HealthLedger*) {}),
                                                  carried));
    }
    CHECK(l.flush());
    CHECK(l.counts() == (Counts{{"radio_fail.rtlsdr.bind", 1}, {"scan_none", 1}}));
    CHECK(l.priorCounts().empty());
    CHECK(readFile(file) == HealthLedger::fileText(id, l.counts()));
    // The next launch would carry exactly that.
    HealthLedger next;
    next.arm(file, id, true);
    CHECK(next.priorCounts() == (Counts{{"radio_fail.rtlsdr.bind", 1}, {"scan_none", 1}}));
}

void testResetsOnlyWhenTheRecordIsAccepted() {
    const fs::path dir = scratch("settle");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));
    writeFile(file, HealthLedger::fileText(id, Counts{{kBusy, 3}}));
    auto l = std::make_shared<HealthLedger>();
    l->arm(file, id, true);
    const Counts carried = l->priorCounts();
    auto stalls = std::make_shared<StallLedger>();

    // A send that fails, a transport that throws, and nothing sent at all, keep it.
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) { return false; }, "https://x/", "{}",
                  cascade::core::settleOnAccept(stalls, 0, l, carried));
    }
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) -> bool { throw 1; }, "https://x/", "{}",
                  cascade::core::settleOnAccept(stalls, 0, l, carried));
    }
    {
        TelemetryReporter r;
        r.sendVia([](const std::string&, const std::string&) { return true; }, "", "{}",
                  cascade::core::settleOnAccept(stalls, 0, l, carried));
    }
    {
        TelemetryReporter r;   // the real transport, refusing a plain-http endpoint before any socket
        r.send("http://127.0.0.1:9/", "{}", cascade::core::settleOnAccept(stalls, 0, l, carried));
    }
    CHECK(l->flush());
    CHECK(l->counts() == (Counts{{kBusy, 3}}));
    CHECK(readFile(file) == HealthLedger::fileText(id, Counts{{kBusy, 3}}));

    // Accepted - and a failure that happens while the send is in flight is NOT
    // lost to the subtraction.
    {
        TelemetryReporter r;
        r.sendVia(
            [&](const std::string&, const std::string&) {
                l->note(kBusy);   // a driver's thread, mid-send
                return true;
            },
            "https://x/", "{}", cascade::core::settleOnAccept(stalls, 0, l, carried));
    }
    CHECK(l->flush());
    CHECK(l->counts() == (Counts{{kBusy, 1}}));
    CHECK(readFile(file) == HealthLedger::fileText(id, Counts{{kBusy, 1}}));

    // Settling everything leaves no file at all; settling more than is held is zero.
    l->settle(Counts{{kBusy, 50}});
    CHECK(l->flush());
    CHECK(l->counts().empty());
    CHECK(!fs::exists(file));
}

void testOffMeansNothingCountedKeptOrSent() {
    const fs::path dir = scratch("off");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));

    // NEVER ARMED (what an opted-out run has) and decided: counts nothing, keeps nothing.
    HealthLedger l;
    l.disarm();
    CHECK(l.decided() && !l.armed());
    l.note(kBusy);
    l.note(health::tokenScanNone());
    CHECK(l.counts().empty());
    CHECK(l.encoded().empty());
    CHECK(l.priorCounts().empty());
    CHECK(!fs::exists(file));
    l.settle(Counts{{kBusy, 1}});
    CHECK(l.counts().empty());

    // Arming needs a real identity.
    l.arm(file, "", false);
    CHECK(!l.armed());
    l.arm(file, "someone@example.com", false);
    CHECK(!l.armed());
    l.note(kBusy);
    CHECK(l.counts().empty());
    CHECK(!fs::exists(file));

    // Armed, counting, on disk - then switched off: forgotten and removed.
    l.arm(file, id, false);
    l.note(kBusy);
    l.note(health::tokenSoundOk(health::AudioApi::Alsa));
    CHECK(l.flush());
    CHECK(fs::exists(file));
    l.disarm();
    CHECK(!l.armed());
    CHECK(l.counts().empty());
    CHECK(waitGone(file));
    l.note(kBusy);                // a failure after the switch is not counted
    l.settle(Counts{{kBusy, 2}});  // and a send completing after it brings nothing back
    CHECK(l.counts().empty());
    CHECK(l.flush());
    CHECK(!fs::exists(file));

    // Switching back ON starts from nothing and reads no file, even one an
    // earlier identity left.
    writeFile(file, HealthLedger::fileText(id, Counts{{"upd_dl", 9}}));
    l.arm(file, cascade::core::newInstallId(), /*loadExisting=*/false);
    CHECK(l.counts().empty());
    fs::remove(file);

    // A run that has not decided anything yet and is then told "off" drops what it held.
    HealthLedger early;
    early.note(kBusy);
    CHECK(countOf(early.counts(), kBusy) == 1);
    early.disarm();
    CHECK(early.counts().empty());
    CHECK(!fs::exists(file));
}

void testDiagnosticsIsTheSecondGate() {
    // The rule the stall count follows: counted only while Usage reporting AND
    // Diagnostics are on.
    const fs::path dir = scratch("diag");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));

    // Diagnostics OFF before the decision: what was held is dropped at arm().
    HealthLedger a;
    a.note(kBusy);
    a.setAllowed(false);
    a.arm(file, id, false);
    CHECK(a.counts().empty());
    a.note(kBusy);
    CHECK(a.counts().empty());
    CHECK(a.flush());
    CHECK(!fs::exists(file));
    // ...and back on, it counts again.
    a.setAllowed(true);
    a.note(kBusy);
    CHECK(countOf(a.counts(), kBusy) == 1);

    // Switched off mid-session: nothing NEW is counted; what is kept stays and is still sent.
    a.setAllowed(false);
    a.note(health::tokenScanNone());
    CHECK(a.counts() == (Counts{{kBusy, 1}}));
    CHECK(a.encoded() == "radio_fail.rtlsdr.busy=1");
}

void testTheFileIsWrittenOffTheCallersThread() {
    // The caller (the GUI thread, or a driver's worker) never waits on the disk:
    // note() returns before the write, and flush() is what waits for it.
    const fs::path dir = scratch("async");
    const std::string id = cascade::core::newInstallId();
    HealthLedger l;
    l.arm(HealthLedger::pathIn(u8(dir)), id, false);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) { l.note(kBusy); }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("health: 200 notes took %.2f ms on the calling thread\n", ms);
    CHECK(l.flush());
    CHECK(readFile(HealthLedger::pathIn(u8(dir))) == HealthLedger::fileText(id, l.counts()));
    CHECK(countOf(l.counts(), kBusy) == 200);
}

void testTheGlobalLedgerIsWhatTheHelpersCountInto() {
    auto g = health::globalLedger();
    g->reset();
    const fs::path dir = scratch("global");
    const std::string id = cascade::core::newInstallId();
    health::noteScanNone();                         // before any decision: held
    g->arm(HealthLedger::pathIn(u8(dir)), id, false);
    health::noteRadioOpen("rtlsdr");
    health::noteRadioData("rtlsdr");
    health::noteRadioFail("rtlsdr", cascade::usb::kInUseError);
    health::noteRadioFail("My Radio", "no such device");
    health::noteSoundOk("Windows WASAPI");
    health::noteSoundFail(health::AudioApi::None, health::SoundReason::NoDevice);
    health::noteUpdateCheckFailed();
    health::noteUpdateDownloadFailed();
    health::noteUpdateVerifyFailed();
    health::noteUpdateRunFailed();
    health::notePluginCatalogueFailed();
    health::notePluginInstallFailed(health::InstallClass::Hash);
    health::notePluginLoadRefused(health::LoadClass::Retired);
    health::noteRecordFailed();
    CHECK(g->counts() == (Counts{{"scan_none", 1}, {"radio_open.rtlsdr", 1}, {"radio_data.rtlsdr", 1},
                                 {"radio_fail.rtlsdr.busy", 1}, {"radio_fail.other.absent", 1},
                                 {"sound_ok.wasapi", 1}, {"sound_fail.none.nodevice", 1},
                                 {"upd_check", 1}, {"upd_dl", 1}, {"upd_verify", 1}, {"upd_run", 1},
                                 {"plug_cat", 1}, {"plug_inst.hash", 1}, {"plug_load.retired", 1},
                                 {"rec_fail", 1}}));
    CHECK(health::sumOf(g->counts(), "radio_fail") == 2);
    CHECK(health::sumOf(g->counts(), "radio_open") == 1);
    CHECK(health::sumOf(g->counts(), "radio") == 0);   // a prefix of a name is not an event
    g->reset();
}

// ---------------------------------------------------------------------------
// The reasons are read off what the drivers REALLY say.
// ---------------------------------------------------------------------------
void testReasonsFromTheDriversOwnSentences() {
    using health::RadioReason;
    struct Case {
        const char* kind;
        std::string said;
        RadioReason want;
    };
    const std::string sessionLost = cascade::source::sdrPlaySessionLostSentence();
    const std::vector<Case> cases = {
        // another program has it: the transports' shared sentence, and the RSPduo's
        {"rtlsdr", cascade::usb::kInUseError, RadioReason::Busy},
        {"hackrf", std::string(cascade::usb::kInUseError) + " or its kernel driver could not be detached",
         RadioReason::Busy},
        {"sdrplay", "this RSPduo is already in use by another application", RadioReason::Busy},
        {"soapy", "usb_claim_interface error -6", RadioReason::Busy},
        // present but not reachable
        {"rtlsdr", std::string("no RTL-SDR is ") + cascade::usb::kBindHint + " on this machine", RadioReason::Bind},
        {"airspy", "WinUsb_Initialize (is the device bound to WinUSB?): The parameter is incorrect.", RadioReason::Bind},
        {"hackrf", "permission denied opening /dev/bus/usb/001/004 (see installer/linux/README.md)", RadioReason::Bind},
        // the driver or service is not there
        {"sdrplay", cascade::source::sdrPlayApiAdvice(false, 0.0f), RadioReason::Driver},
        {"sdrplay", "the SDRplay service did not answer: sdrplay_api_Fail. Check that the SDRplay API service is running.",
         RadioReason::Driver},
        {"sdrplay", sessionLost, RadioReason::Driver},
        {"rtlsdr", "native USB radio support needs WinUSB (Windows) or usbfs (Linux) in this build", RadioReason::Driver},
        // it did not answer
        {"rtlsdr", "the radio did not answer its first register write", RadioReason::Timeout},
        {"soapy", "the radio's driver is busy or not answering; try again", RadioReason::Timeout},
        {"hackrf", "the HackRF did not answer its board id", RadioReason::Timeout},
        // a rate it will not run
        {"rtlsdr", "the resampler would not accept the default sample rate", RadioReason::Rate},
        // it is not there
        {"sdrplay", "no SDRplay device is connected", RadioReason::Absent},
        {"sdrplay", "no SDRplay device matches 'serial=0001'", RadioReason::Absent},
        {"rtlsdr", "there is no RTL-SDR at that index", RadioReason::Absent},
        {"rtlsdr", "no RTL-SDR with serial 00000001 is present", RadioReason::Absent},
        {"rtlsdr", "the radio is no longer present", RadioReason::Absent},
        {"pluto", "nothing at the radio's address:30431 answered as an IIO daemon", RadioReason::Absent},
        {"soapy", "SoapySDR::Device::make() no match for args: driver=uhd", RadioReason::Absent},
        {"soundcard", "the card is not connected", RadioReason::Absent},
        // a vendor stack's own words
        {"sdrplay", "SDRplay SelectDevice failed: sdrplay_api_Fail", RadioReason::Vendor},
        {"soapy", "SoapySDR threw: something unforeseen (args: driver=lime)", RadioReason::Vendor},
        // nothing recognised: other, which is the denominator staying right
        {"hackrf", "something unforeseen", RadioReason::Other},
        {"rtlsdr", "", RadioReason::Other},
        {"other", "the radio would not open", RadioReason::Other},
    };
    for (const Case& c : cases) {
        const RadioReason got = health::classifyRadioOpen(c.kind, c.said);
        if (got != c.want) {
            std::printf("FAIL %s said \"%s\": %s, wanted %s\n", c.kind, c.said.c_str(),
                        health::radioReasonWord(got), health::radioReasonWord(c.want));
        }
        CHECK(got == c.want);
    }
    // The text is classified and DROPPED: whatever is in it, the token has only vocabulary words.
    const std::string t = health::tokenRadioFail(
        "rtlsdr", health::classifyRadioOpen("rtlsdr", "no RTL-SDR with serial EDR04ZDB2 is present at C:\\Users\\someone"));
    CHECK(t == "radio_fail.rtlsdr.absent");
    CHECK(t.find("EDR04ZDB2") == std::string::npos);
}

void testTheTranslatedSentencesAreClassifiedInTheLanguageInForce() {
    // A few driver sentences go through tr(); a German window hands the
    // classifier German. Each is matched in the language in force as well.
    const std::string english = cascade::source::sdrPlayApiAdvice(false, 0.0f);
    cascade::i18n::setLanguage("de");
    const std::string german = cascade::source::sdrPlayApiAdvice(false, 0.0f);
    CHECK(german != english);   // the catalogue really translates it
    CHECK(health::classifyRadioOpen("sdrplay", german) == health::RadioReason::Driver);
    cascade::i18n::setLanguage("en");
    CHECK(health::classifyRadioOpen("sdrplay", english) == health::RadioReason::Driver);

    // THE KEYS THE CLASSIFIER KNOWS ARE THE DRIVERS' OWN STRINGS: each is a key in
    // the German catalogue, so a rewording in the driver that is not made here
    // fails this test instead of silently turning a class into "other".
    const nlohmann::json de = nlohmann::json::parse(sourceFile("resources/lang/de.json"));
    const std::string aorBound =
        "The AOR receiver's I/Q interface (USB 08D0:A001) is plugged in but is not bound to "
        "WinUSB - AOR's own driver (AorAlpha) and WinUSB cannot both own it. Run Zadig, select the "
        "AOR I/Q interface, choose WinUSB and click Replace Driver, then try again.";
    const std::string aorMissing =
        "No AOR I/Q interface (USB 08D0:A001) was found. Check that the receiver's I/Q USB cable "
        "is connected and the receiver is switched on.";
    for (const std::string& key : {english, aorBound, aorMissing}) {
        CHECK(de["strings"].contains(key));
    }
    cascade::i18n::setLanguage("de");
    CHECK(health::classifyRadioOpen("aor", cascade::i18n::tr(aorBound.c_str())) == health::RadioReason::Bind);
    CHECK(health::classifyRadioOpen("aor", cascade::i18n::tr(aorMissing.c_str())) == health::RadioReason::Absent);
    cascade::i18n::setLanguage("en");
    CHECK(health::classifyRadioOpen("aor", aorBound) == health::RadioReason::Bind);
    CHECK(health::classifyRadioOpen("aor", aorMissing) == health::RadioReason::Absent);
}

// ---------------------------------------------------------------------------
// The record, the document and the Worker say what the code says.
// ---------------------------------------------------------------------------
void testTheRecordCarriesOnlyVocabulary() {
    cascade::core::TelemetryReport r;
    r.installId = cascade::core::newInstallId();
    r.appVersion = "0.99.64";
    r.health = "radio_fail.rtlsdr.busy=2,serial=00000001,My Radio=1,scan_none=1";
    const nlohmann::json j = nlohmann::json::parse(r.toJson());
    CHECK(j.contains("health"));
    CHECK(j["health"] == "scan_none=1,radio_fail.rtlsdr.busy=2");
    r.health.clear();
    CHECK(nlohmann::json::parse(r.toJson())["health"] == "");     // always sent: "nothing failed" is a fact
    // withHealth sets it on a stored record and leaves a non-record alone.
    CHECK(nlohmann::json::parse(health::withHealth("{\"id\":\"x\"}", "upd_dl=1,junk"))["health"] == "upd_dl=1");
    CHECK(health::withHealth("not json", "upd_dl=1") == "not json");
    CHECK(health::withHealth("[1]", "upd_dl=1") == "[1]");
}

// One bold-led table of PRIVACY.md ("**Drivers.** ..." and the table after it): the
// code-span words in its first column.
std::vector<std::string> docWords(const std::string& doc, const std::string& heading) {
    std::vector<std::string> words;
    const std::size_t at = doc.find("**" + heading + ".**");
    if (at == std::string::npos) { return words; }
    std::istringstream lines(doc.substr(at));
    std::string line;
    bool inTable = false;
    std::getline(lines, line);   // the heading line itself
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
        if (line.rfind("|", 0) != 0) {
            if (inTable) { break; }
            continue;
        }
        inTable = true;
        if (line.rfind("|---", 0) == 0 || line.rfind("| Word", 0) == 0 || line.rfind("| Event", 0) == 0) { continue; }
        const std::size_t a = line.find('`');
        const std::size_t b = line.find('`', a + 1);
        if (a == std::string::npos || b == std::string::npos) { continue; }
        words.push_back(line.substr(a + 1, b - a - 1));
    }
    return words;
}

std::vector<std::string> sortedCopy(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

// The vocabulary's own lists, by the heading PRIVACY.md and the Worker use for them.
struct Lists {
    std::vector<std::string> events, drivers, radioReasons, apis, soundReasons, installClasses, loadClasses;
};

Lists codeLists() {
    Lists l;
    for (const health::VocabularyEntry& e : health::vocabulary()) {
        l.events.push_back(e.name);
        if (e.name == "radio_open") { l.drivers = e.qualifiers[0]; }
        if (e.name == "radio_fail") { l.radioReasons = e.qualifiers[1]; }
        if (e.name == "sound_ok") { l.apis = e.qualifiers[0]; }
        if (e.name == "sound_fail") { l.soundReasons = e.qualifiers[1]; }
        if (e.name == "plug_inst") { l.installClasses = e.qualifiers[0]; }
        if (e.name == "plug_load") { l.loadClasses = e.qualifiers[0]; }
    }
    return l;
}

void testPrivacyListsEveryWordOfTheVocabulary() {
    const std::string whole = sourceFile("PRIVACY.md");
    const std::size_t section = whole.find("### Failures that are not crashes");
    CHECK(section != std::string::npos);
    // The tables of that section only: no other part of the document is read as one.
    const std::string doc = section == std::string::npos ? std::string() : whole.substr(section);
    const Lists code = codeLists();
    auto same = [&](const char* heading, const std::vector<std::string>& want) {
        const std::vector<std::string> got = docWords(doc, heading);
        if (sortedCopy(got) != sortedCopy(want)) {
            std::printf("FAIL PRIVACY.md \"%s\" lists:", heading);
            for (const auto& w : got) { std::printf(" %s", w.c_str()); }
            std::printf("\n     the code has:");
            for (const auto& w : want) { std::printf(" %s", w.c_str()); }
            std::printf("\n");
        }
        CHECK(!want.empty());
        CHECK(sortedCopy(got) == sortedCopy(want));
        // no word twice in one table
        CHECK(std::set<std::string>(got.begin(), got.end()).size() == got.size());
    };
    // The events table names events with their qualifier slots ("radio_fail.<driver>.<why>");
    // the event is the part before the first dot, and the slots must match the grammar.
    {
        const std::vector<std::string> rows = docWords(doc, "Events");
        std::vector<std::string> names;
        for (const std::string& row : rows) {
            const std::size_t dot = row.find('.');
            const std::string name = row.substr(0, dot);
            names.push_back(name);
            int slots = 0;
            for (std::size_t p = row.find('<'); p != std::string::npos; p = row.find('<', p + 1)) { ++slots; }
            for (const health::VocabularyEntry& e : health::vocabulary()) {
                if (e.name == name && static_cast<std::size_t>(slots) != e.qualifiers.size()) {
                    std::printf("FAIL PRIVACY.md writes %s with %d slot(s); the code has %zu\n", row.c_str(),
                                slots, e.qualifiers.size());
                    CHECK(static_cast<std::size_t>(slots) == e.qualifiers.size());
                }
            }
        }
        CHECK(sortedCopy(names) == sortedCopy(code.events));
    }
    same("Drivers", code.drivers);
    same("Why a radio would not open", code.radioReasons);
    same("Audio host APIs", code.apis);
    same("Why the sound output would not open", code.soundReasons);
    same("Plugin install failures", code.installClasses);
    same("Plugin refusals", code.loadClasses);
}

void testTheWorkerCarriesTheSameVocabulary() {
    const std::string js = sourceFile("telemetry-worker/worker.js");
    const std::size_t begin = js.find("// BEGIN HEALTH VOCABULARY");
    const std::size_t end = js.find("// END HEALTH VOCABULARY");
    CHECK(begin != std::string::npos && end != std::string::npos && end > begin);
    if (begin == std::string::npos || end == std::string::npos || end < begin) { return; }
    const std::size_t open = js.find('[', begin);
    const std::size_t close = js.rfind(']', end);
    const nlohmann::json w = nlohmann::json::parse(js.substr(open, close - open + 1), nullptr, false);
    CHECK(w.is_array());
    if (!w.is_array()) { return; }
    const std::vector<health::VocabularyEntry>& v = health::vocabulary();
    CHECK(w.size() == v.size());
    for (std::size_t i = 0; i < v.size() && i < w.size(); ++i) {
        CHECK(w[i]["name"] == v[i].name);
        CHECK(w[i]["qualifiers"].size() == v[i].qualifiers.size());
        for (std::size_t q = 0; q < v[i].qualifiers.size() && q < w[i]["qualifiers"].size(); ++q) {
            // The same words IN THE SAME ORDER: the order is the written order of a record.
            CHECK(w[i]["qualifiers"][q].get<std::vector<std::string>>() == v[i].qualifiers[q]);
        }
    }
    // The bounds the Worker enforces are the application's.
    auto constant = [&](const char* name) {
        const std::size_t at = js.find(std::string("const ") + name + " = ");
        return at == std::string::npos ? -1L : std::strtol(js.c_str() + at + std::strlen("const ") + std::strlen(name) + 3, nullptr, 10);
    };
    CHECK(constant("MAX_HEALTH_CHARS") == static_cast<long>(health::kMaxEncodedBytes));
    CHECK(constant("MAX_HEALTH_TOKENS") == static_cast<long>(health::kMaxTokens));
    CHECK(constant("MAX_HEALTH_RADIO_FAIL_TOKENS") == static_cast<long>(health::kMaxRadioFailTokens));
    CHECK(constant("MAX_HEALTH_COUNT") == static_cast<long>(health::kMaxCount));
}

}  // namespace

int main() {
    testEveryTokenIsLegalAndNothingElseIs();
    testBuildersCannotCarryFreeText();
    testEncodingIsStrictAndBounded();
    testCountsOnceAndOftenAsDeclared();
    testBoundsHold();
    testACountSurvivesACrash();
    testTheRecordCarriesEarlierSessionsAndNotThisOne();
    testResetsOnlyWhenTheRecordIsAccepted();
    testOffMeansNothingCountedKeptOrSent();
    testDiagnosticsIsTheSecondGate();
    testTheFileIsWrittenOffTheCallersThread();
    testTheGlobalLedgerIsWhatTheHelpersCountInto();
    testReasonsFromTheDriversOwnSentences();
    testTheTranslatedSentencesAreClassifiedInTheLanguageInForce();
    testTheRecordCarriesOnlyVocabulary();
    testPrivacyListsEveryWordOfTheVocabulary();
    testTheWorkerCarriesTheSameVocabulary();
    return testSummary("test_health_events");
}
