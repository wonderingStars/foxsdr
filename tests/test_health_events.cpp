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

#include "core/frame_timing.hpp"
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
    // 1 scan + 14 + 14 + 112 (driver x reason) + 11 + 55 (api x reason) + 4 update
    // + 1 catalogue + 4 + 3 plugin + 1 recording = 220 failure tokens (0.99.66: the
    // thirteenth driver word, hydrasdr, added one open, one data and eight fail
    // tokens; 0.99.70: the fourteenth, rtltcp, adds the same ten again); then 54
    // slow (18 countable scopes x 3 tiers) and 13 recovered.
    CHECK(all.size() == 220 + 54 + 13);
    std::set<std::string> unique(all.begin(), all.end());
    CHECK(unique.size() == all.size());
    for (const std::string& t : all) {
        if (!health::validToken(t)) { std::printf("FAIL %s is in allTokens() and not valid\n", t.c_str()); }
        CHECK(health::validToken(t));
        CHECK(t.size() <= 29);
        for (char c : t) {
            CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-');
        }
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
                              "pluto", "aor", "hydrasdr", "rtltcp", "soapy", "soundcard"}) {
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
        if (t.rfind("slow.", 0) == 0 || t.rfind("recovered.", 0) == 0) { continue; }
        (t.rfind("radio_fail.", 0) == 0 ? fails : others).push_back(t);
    }
    auto longest = [](std::vector<std::string>& v, std::size_t n) {
        std::stable_sort(v.begin(), v.end(),
                         [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        v.resize(n);
    };
    longest(fails, health::kMaxRadioFailTokens);
    longest(others, health::kMaxBaseTokens - health::kMaxRadioFailTokens);
    Counts worst;
    for (const auto& t : fails) { worst[t] = 999; }
    for (const auto& t : others) { worst[t] = 999; }
    CHECK(worst.size() == health::kMaxBaseTokens);
    const std::string widest = health::encode(worst);
    CHECK(health::decode(widest, back) && back == worst);   // none was cut for length
    CHECK(widest.size() <= health::kMaxEncodedBytes);
    std::printf("health: the widest legal failure record is %zu bytes (cap %zu)\n", widest.size(),
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
        if (t.rfind("radio_fail.", 0) == 0 || t.rfind("slow.", 0) == 0 ||
            t.rfind("recovered.", 0) == 0) {
            continue;
        }
        l.note(t);
        ++sent;
    }
    CHECK(sent > health::kMaxBaseTokens);
    CHECK(l.counts().size() == health::kMaxBaseTokens);

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
// SLOW FRAMES and RECOVERIES (0.99.65, the second pass over this vocabulary).
// ---------------------------------------------------------------------------
using cascade::core::FrameScope;
using cascade::core::kFrameScopeCount;
using cascade::core::kFrameScopeNames;

std::string slowTok(int scope, int tier) {
    static const char* const tiers[] = {"250ms", "1s", "5s"};
    return std::string("slow.") + kFrameScopeNames[scope] + "." + tiers[tier];
}

// What a record of `c` carries, read back through the strict decoder.
Counts carriedBy(const Counts& c) {
    Counts back;
    const std::string text = health::encode(c);
    if (!health::decode(text, back)) { std::printf("FAIL encode wrote text decode refuses: %s\n", text.c_str()); }
    return back;
}

void testSlowFrameWordsAreTheFrameTimersOwn() {
    // THE SCOPE WORDS ARE kFrameScopeNames, spelt as the frame timer spells them
    // - hyphens included - minus `user-wait`, which is time a person set the pace
    // of and is never counted.
    std::vector<std::string> want;
    for (int i = 0; i < kFrameScopeCount; ++i) {
        if (i != static_cast<int>(FrameScope::UserWait)) { want.push_back(kFrameScopeNames[i]); }
    }
    CHECK(want.size() == 18);
    CHECK(health::slowScopeWords() == want);
    const std::vector<std::string>& scopes = health::slowScopeWords();
    CHECK(std::find(scopes.begin(), scopes.end(), "user-wait") == scopes.end());
    for (const char* w : {"other", "startup", "plugin-panels", "plugins-reload", "frame-start", "pre-draw",
                          "user-wait"}) {
        const bool present = std::find(scopes.begin(), scopes.end(), w) != scopes.end();
        CHECK(present == (std::string(w) != "user-wait"));
    }
    // THE TIER WORDS ARE THE TIER WIDTHS the timer uses, in order.
    std::vector<std::string> tiers;
    for (int t = 0; t < cascade::core::kSlowFrameTiers; ++t) {
        const long long ns = cascade::core::kSlowFrameTierNs[t];
        tiers.push_back(ns % 1'000'000'000LL == 0 ? std::to_string(ns / 1'000'000'000LL) + "s"
                                                  : std::to_string(ns / 1'000'000LL) + "ms");
    }
    CHECK(tiers == (std::vector<std::string>{"250ms", "1s", "5s"}));
    CHECK(health::slowTierWords() == tiers);

    // Every scope x tier is a legal token, built the same way by tokenSlowFrame.
    int n = 0;
    for (int s = 0; s < kFrameScopeCount; ++s) {
        for (int t = 0; t < 3; ++t) {
            if (s == static_cast<int>(FrameScope::UserWait)) {
                CHECK(health::tokenSlowFrame(s, t).empty());
                CHECK(!health::validToken(slowTok(s, t)));
                continue;
            }
            const std::string tok = slowTok(s, t);
            if (!health::validToken(tok)) { std::printf("FAIL %s is not a legal token\n", tok.c_str()); }
            CHECK(health::validToken(tok));
            CHECK(health::tokenSlowFrame(s, t) == tok);
            ++n;
        }
    }
    CHECK(n == 54);
    // What is not a token: the user's own time, a tier that is not one of the three,
    // a missing or extra part, the wrong case, a scope the timer does not have.
    for (const char* bad : {"slow.user-wait.250ms", "slow.user-wait.1s", "slow.rail.2s", "slow.rail.250", "slow.rail",
                            "slow", "slow.rail.250ms.x", "slow..250ms", "slow.rail.", "slow.Rail.250ms",
                            "slow.rail.250MS", "slow.rail.0ms", "slow.plugin_panels.250ms", "slow.plugins.250ms",
                            "slow.rail.250ms ", " slow.rail.250ms", "slow.rail.250ms=1", "slow.rail.1S",
                            "slow.all.250ms"}) {
        if (health::validToken(bad)) { std::printf("FAIL \"%s\" was accepted\n", bad); }
        CHECK(!health::validToken(bad));
    }
    // Out of range builds nothing.
    CHECK(health::tokenSlowFrame(-1, 0).empty());
    CHECK(health::tokenSlowFrame(kFrameScopeCount, 0).empty());
    CHECK(health::tokenSlowFrame(0, -1).empty());
    CHECK(health::tokenSlowFrame(0, 3).empty());
    // slow, then recovered, are LAST in the written order.
    const std::vector<health::VocabularyEntry>& v = health::vocabulary();
    CHECK(v.size() >= 2 && v[v.size() - 2].name == "slow" && v.back().name == "recovered");
    CHECK(v[v.size() - 2].qualifiers.size() == 2 && v.back().qualifiers.size() == 1);
    CHECK(health::encode({{slowTok(0, 0), 1}, {"rec_fail", 1}, {"recovered.audio", 1}, {"scan_none", 1}}) ==
          "scan_none=1,rec_fail=1," + slowTok(0, 0) + "=1,recovered.audio=1");
}

void testSlowFramesCountThroughTheLedger() {
    HealthLedger l;
    l.arm("", cascade::core::newInstallId(), false);
    // A slow frame is one count each time (it is not once a session: how many is the
    // question), in the token of its scope and tier.
    for (int i = 0; i < 3; ++i) { l.note(slowTok(static_cast<int>(FrameScope::Rail), 0)); }
    l.note(slowTok(static_cast<int>(FrameScope::Rail), 1));
    l.note(slowTok(static_cast<int>(FrameScope::PluginsReload), 2));
    CHECK(l.counts() == (Counts{{"slow.rail.250ms", 3}, {"slow.rail.1s", 1}, {"slow.plugins-reload.5s", 1}}));
    // user-wait is refused at the ledger too.
    l.note("slow.user-wait.250ms");
    CHECK(l.counts().size() == 3);
    // Through the helper the frame timer calls: the scope index and the tier.
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    health::noteSlowFrame(static_cast<int>(FrameScope::Spectrum), 0);
    health::noteSlowFrame(static_cast<int>(FrameScope::Spectrum), 0);
    health::noteSlowFrame(static_cast<int>(FrameScope::Startup), 2);
    health::noteSlowFrame(static_cast<int>(FrameScope::UserWait), 0);   // never counted
    health::noteSlowFrame(kFrameScopeCount, 0);
    health::noteSlowFrame(0, 7);
    CHECK(g->counts() == (Counts{{"slow.spectrum.250ms", 2}, {"slow.startup.5s", 1}}));
    g->reset();
}

void testSlowTokensPerRecordKeepTheWorst() {
    const int kStart = 0;
    // 1. Exactly the cap: all kept (the boundary).
    {
        Counts c;
        for (int i = 0; i < 8; ++i) { c[slowTok(i, 0)] = 1; }
        CHECK(carriedBy(c) == c);
    }
    // 2. One more, same tier, counts 1..9: the LOWEST COUNT goes.
    {
        Counts c;
        for (int i = 0; i < 9; ++i) { c[slowTok(i, 0)] = static_cast<std::uint32_t>(i + 1); }
        Counts want = c;
        want.erase(slowTok(0, 0));
        CHECK(carriedBy(c) == want);
    }
    // 3. One more, same tier, same count: the LATEST IN THE WRITTEN ORDER goes.
    {
        Counts c;
        for (int i = 0; i < 9; ++i) { c[slowTok(kStart + i, 0)] = 3; }
        Counts want = c;
        want.erase(slowTok(8, 0));
        CHECK(carriedBy(c) == want);
    }
    // 4. A HIGHER TIER beats a higher count: one 5 s frame against eight stutters of
    //    999, the 5 s is kept and the last stutter in the written order goes.
    {
        Counts c;
        for (int i = 0; i < 8; ++i) { c[slowTok(i, 0)] = 999; }
        c[slowTok(static_cast<int>(FrameScope::Other), 2)] = 1;
        Counts want = c;
        want.erase(slowTok(7, 0));
        CHECK(carriedBy(c) == want);
        CHECK(carriedBy(c).count(slowTok(static_cast<int>(FrameScope::Other), 2)) == 1);
    }
    // 5. ...and a 1 s frame beats a stutter the same way, and loses to a 5 s one.
    {
        Counts c;
        for (int i = 0; i < 6; ++i) { c[slowTok(i, 0)] = 999; }
        c[slowTok(9, 1)] = 1;
        c[slowTok(10, 2)] = 1;
        c[slowTok(11, 0)] = 1;      // the weakest: the 9th, a stutter with count 1
        Counts want = c;
        want.erase(slowTok(11, 0));
        CHECK(carriedBy(c) == want);
    }
    // 6. Of all 54 tokens present at count 1: the eight 5 s ones that come first in the order.
    {
        Counts c;
        for (int s = 0; s < kFrameScopeCount; ++s) {
            if (s == static_cast<int>(FrameScope::UserWait)) { continue; }
            for (int t = 0; t < 3; ++t) { c[slowTok(s, t)] = 1; }
        }
        CHECK(c.size() == 54);
        Counts want;
        int taken = 0;
        for (int s = 0; s < kFrameScopeCount && taken < 8; ++s) {
            if (s == static_cast<int>(FrameScope::UserWait)) { continue; }
            want[slowTok(s, 2)] = 1;
            ++taken;
        }
        CHECK(carriedBy(c) == want);
    }
    // 7. The cap is on `slow` alone: it never takes a failure's place, and a failure never takes its.
    {
        Counts c = {{"scan_none", 1}, {"radio_fail.rtlsdr.busy", 2}, {"upd_dl", 1}};
        for (int i = 0; i < 12; ++i) { c[slowTok(i, 0)] = 1; }
        const Counts back = carriedBy(c);
        CHECK(back.size() == 3 + health::kMaxSlowTokens);
        CHECK(back.count("scan_none") == 1 && back.count("radio_fail.rtlsdr.busy") == 1 && back.count("upd_dl") == 1);
    }
    // 8. The ledger KEEPS what a record cannot carry, so nothing is lost by the cap, and
    //    the same text out of sanitise() is the same selection, in any order.
    {
        HealthLedger l;
        l.arm("", cascade::core::newInstallId(), false);
        for (int i = 0; i < 12; ++i) { l.note(slowTok(i, i % 3)); }
        CHECK(l.counts().size() == 12);
        CHECK(carriedBy(l.counts()).size() == health::kMaxSlowTokens);
        const Counts held = l.counts();
        std::string text;
        for (const auto& kv : held) { text += (text.empty() ? "" : ",") + kv.first + "=" + std::to_string(kv.second); }
        std::string reversed;
        for (auto it = held.rbegin(); it != held.rend(); ++it) {
            reversed += (reversed.empty() ? "" : ",") + it->first + "=" + std::to_string(it->second);
        }
        CHECK(health::sanitise(text) == health::encode(l.counts()));
        CHECK(health::sanitise(reversed) == health::encode(l.counts()));
    }
    // decode is strict about the cap: nine slow tokens in one record is not what encode writes.
    {
        std::string nine;
        for (int i = 0; i < 9; ++i) { nine += (nine.empty() ? "" : ",") + slowTok(i, 0) + "=1"; }
        Counts out;
        CHECK(!health::decode(nine, out));
        CHECK(out.empty());
        std::string eight = nine.substr(0, nine.rfind(','));
        CHECK(health::decode(eight, out) && out.size() == 8);
    }
}

void testRecoveredWordsAndCap() {
    const std::vector<std::string> words = {"audio",    "reopen",    "srcthread", "vendorcall", "ringdrop",
                                            "dspexc", "cfgsave",  "enumchild", "pluginapi",  "webroute",
                                            "patchload", "sdrenum",  "sdrlost"};
    CHECK(health::recoveredWords() == words);
    CHECK(health::kRecoveredCount == words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        const health::Recovered r = static_cast<health::Recovered>(i);
        CHECK(words[i] == health::recoveredWord(r));
        CHECK(health::tokenRecovered(r) == "recovered." + words[i]);
        CHECK(health::validToken("recovered." + words[i]));
        // one fixed lower-case word: letters only
        for (char c : words[i]) { CHECK(c >= 'a' && c <= 'z'); }
    }
    for (const char* bad : {"recovered", "recovered.", "recovered.audio.x", "recovered.Audio", "recovered.unknown",
                            "recovered.audio=1", "recover.audio", "recovered.src-thread", "recovered.audio "}) {
        if (health::validToken(bad)) { std::printf("FAIL \"%s\" was accepted\n", bad); }
        CHECK(!health::validToken(bad));
    }
    // Once a session for what repeats by itself; once an occurrence for the rest.
    const std::set<std::string> once = {"audio", "reopen", "ringdrop", "cfgsave", "enumchild", "pluginapi",
                                        "webroute", "sdrenum", "sdrlost"};
    for (std::size_t i = 0; i < words.size(); ++i) {
        CHECK(health::recoveredOncePerSession(static_cast<health::Recovered>(i)) == (once.count(words[i]) == 1));
    }
    HealthLedger l;
    l.arm("", cascade::core::newInstallId(), false);
    for (int i = 0; i < 5; ++i) {
        l.note("recovered.audio");
        l.note("recovered.srcthread");
    }
    l.note("recovered.vendorcall", 4);
    l.note("recovered.ringdrop", 4);
    CHECK(l.counts() == (Counts{{"recovered.audio", 1}, {"recovered.srcthread", 5},
                                {"recovered.vendorcall", 4}, {"recovered.ringdrop", 1}}));

    // THE CAP: at most 8 distinct recovered tokens in a record. At the boundary all 8 stay...
    Counts c;
    for (int i = 0; i < 8; ++i) { c["recovered." + words[static_cast<std::size_t>(i)]] = 1; }
    CHECK(carriedBy(c) == c);
    // ...with a ninth the higher COUNTS stay (counts 1..9: the 1 goes)...
    Counts nine;
    for (int i = 0; i < 9; ++i) { nine["recovered." + words[static_cast<std::size_t>(i)]] = static_cast<std::uint32_t>(i + 1); }
    Counts want = nine;
    want.erase("recovered.audio");
    CHECK(carriedBy(nine) == want);
    // ...and with equal counts the earlier in the written order stay.
    Counts all;
    for (const std::string& w : words) { all["recovered." + w] = 2; }
    Counts first8;
    for (int i = 0; i < 8; ++i) { first8["recovered." + words[static_cast<std::size_t>(i)]] = 2; }
    CHECK(carriedBy(all) == first8);
    // It is its own family: it does not take a failure's place, and 24 failures leave it its 8.
    Counts mixed = {{"scan_none", 1}};
    for (const std::string& w : words) { mixed["recovered." + w] = 1; }
    CHECK(carriedBy(mixed).size() == 1 + health::kMaxRecoveredTokens);
    // decode refuses nine.
    std::string text;
    for (int i = 0; i < 9; ++i) { text += (text.empty() ? "" : ",") + std::string("recovered.") + words[static_cast<std::size_t>(i)] + "=1"; }
    Counts out;
    CHECK(!health::decode(text, out));
}

void testARecordCarriesTheSelectionAndSettlesExactlyThat() {
    // The start-up record carries the SELECTION of the earlier sessions' counts, and
    // only that selection is taken off when the server accepts it: what the cap left
    // out stays in the ledger and goes with the next record.
    const fs::path dir = scratch("selection");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));
    Counts before = {{"scan_none", 1}, {"upd_dl", 2}};
    for (int i = 0; i < 12; ++i) { before[slowTok(i, 0)] = static_cast<std::uint32_t>(i + 1); }
    writeFile(file, HealthLedger::fileText(id, before));
    auto l = std::make_shared<HealthLedger>();
    l->arm(file, id, true);
    CHECK(l->priorCounts().size() == before.size());      // the file's whole content is read back

    const std::string pending = "{\"id\":\"" + id + "\",\"v\":\"0.99.63\"}";
    std::string json;
    std::uint64_t carriedStalls = 0;
    Counts carried;
    StallLedger stalls;
    CHECK(cascade::core::prepareStartupRecord("", pending, stalls, *l, json, carriedStalls, carried));
    const std::string text = nlohmann::json::parse(json)["health"].get<std::string>();
    Counts sent;
    CHECK(health::decode(text, sent));
    CHECK(sent == carried);                                // what it settles is what it sent
    CHECK(sent.size() == 2 + health::kMaxSlowTokens);
    CHECK(sent.count(slowTok(0, 0)) == 0);                 // the lowest counts were left out
    CHECK(sent.count(slowTok(11, 0)) == 1);

    l->settle(carried);
    CHECK(l->flush());
    Counts left = l->counts();
    CHECK(left.size() == 12 - health::kMaxSlowTokens);     // the four left out are still kept
    CHECK(left.count(slowTok(0, 0)) == 1 && left.count(slowTok(3, 0)) == 1 && left.count(slowTok(4, 0)) == 0);
    CHECK(readFile(file) == HealthLedger::fileText(id, left));
}

void testTheLedgersFileHoldsEverythingItMayCarry() {
    // The ledger keeps what a record cannot carry (every `slow` and `recovered` kind it met), so the
    // FILE can be wider than any record: 24 of the longest failure kinds, every slow kind and every
    // recovered word, all at 999. arm() reads at most 4096 bytes of it back; one that did not fit would
    // be cut short and silently lose counts.
    std::vector<std::string> fails, others;
    for (const std::string& t : health::allTokens()) {
        if (t.rfind("slow.", 0) == 0 || t.rfind("recovered.", 0) == 0) { continue; }
        (t.rfind("radio_fail.", 0) == 0 ? fails : others).push_back(t);
    }
    auto longest = [](std::vector<std::string>& v, std::size_t n) {
        std::stable_sort(v.begin(), v.end(),
                         [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        v.resize(n);
    };
    longest(fails, health::kMaxRadioFailTokens);
    longest(others, health::kMaxBaseTokens - health::kMaxRadioFailTokens);
    Counts widest;
    for (const std::string& t : fails) { widest[t] = 999; }
    for (const std::string& t : others) { widest[t] = 999; }
    for (const std::string& t : health::allTokens()) {
        if (t.rfind("slow.", 0) == 0 || t.rfind("recovered.", 0) == 0) { widest[t] = 999; }
    }
    CHECK(widest.size() == health::kMaxBaseTokens + 54 + health::kRecoveredCount);
    const std::string id = cascade::core::newInstallId();
    const std::string text = HealthLedger::fileText(id, widest);
    std::printf("health: the widest ledger file is %zu bytes (read back with a 4096-byte buffer)\n", text.size());
    CHECK(text.size() < 4096);
    const fs::path dir = scratch("widestfile");
    const std::string file = HealthLedger::pathIn(u8(dir));
    writeFile(file, text);
    HealthLedger l;
    l.arm(file, id, true);
    CHECK(l.priorCounts() == widest);
    // ...and what a record carries of it is the selection, never more than its own cap.
    CHECK(health::encode(l.priorCounts()).size() <= health::kMaxEncodedBytes);
    CHECK(carriedBy(l.priorCounts()).size() <= health::kMaxRecordTokens);
    // THE WIDEST RECORD: the 832-byte cap (what an older Worker accepts) cuts the newest families first and
    // never displaces a failure token; what it leaves out stays in the ledger for the next record.
    const Counts sent = carriedBy(widest);
    std::size_t failureKept = 0, slowKept = 0, recoveredKept = 0;
    for (const auto& kv : sent) {
        if (kv.first.rfind("slow.", 0) == 0) { ++slowKept; }
        else if (kv.first.rfind("recovered.", 0) == 0) { ++recoveredKept; }
        else { ++failureKept; }
    }
    std::printf("health: the widest legal record carries %zu failure, %zu slow and %zu recovered tokens in %zu bytes (cap %zu)\n",
                failureKept, slowKept, recoveredKept, health::encode(widest).size(), health::kMaxEncodedBytes);
    CHECK(failureKept == health::kMaxBaseTokens);
    CHECK(slowKept <= health::kMaxSlowTokens && recoveredKept <= health::kMaxRecoveredTokens);
    CHECK(health::encode(widest).size() <= health::kMaxEncodedBytes);
}

std::atomic<int> gWriterStalls{0};
void stallTheWriter() {
    ++gWriterStalls;
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
}

void testASlowFrameNeverWaitsOnTheDisk() {
    // THE GUI THREAD COUNTS A SLOW FRAME (FrameTimer::commit -> noteSlowFrame ->
    // HealthLedger::note) and must not wait on a disk. With the WRITER made to take
    // 700 ms - a slow share, a laptop disk spinning up - a note that wrote the file
    // itself would take at least that; this is what separates "handed to its own
    // thread" from "wrote it". The figure that moves is the time the CALLER spent.
    const fs::path dir = scratch("stalled");
    const std::string id = cascade::core::newInstallId();
    const std::string file = HealthLedger::pathIn(u8(dir));
    gWriterStalls = 0;
    HealthLedger::setWriteHookForTest(&stallTheWriter);
    HealthLedger l;
    l.arm(file, id, false);
    double worstMs = 0.0, totalMs = 0.0;
    const int kNotes = 300;
    for (int i = 0; i < kNotes; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        l.note(slowTok(i % kFrameScopeCount == static_cast<int>(FrameScope::UserWait) ? 0 : i % kFrameScopeCount, i % 3));
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worstMs = std::max(worstMs, ms);
        totalMs += ms;
        if (i == 2) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }   // the writer is inside its stall now
        if (totalMs > 2000.0) { break; }    // a note that waits on the disk: report it, do not sit out 300 stalls
    }
    std::printf("health: %d slow-frame notes with the writer stalled 700 ms: total %.2f ms, worst single %.3f ms\n",
                kNotes, totalMs, worstMs);
    CHECK(totalMs < 350.0);      // a synchronous write is >= 700 ms for the first note alone
    CHECK(worstMs < 300.0);
    CHECK(l.flush(10000));
    HealthLedger::setWriteHookForTest(nullptr);
    CHECK(gWriterStalls.load() >= 1);                       // the hook really ran on the writer
    CHECK(readFile(file) == HealthLedger::fileText(id, l.counts()));
    CHECK(l.counts().size() > health::kMaxSlowTokens);      // the ledger kept every kind it met
}

void testTheWatchCountsWhatTheCountersSay() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    health::RecoveryWatch w;
    health::RecoveryReadings r;
    r.sourceThreadsAbandoned = 5;
    r.vendorCallsAbandoned = 7;
    r.ringDroppedSamples = 100;
    r.dspThreadExceptions = 2;
    // The first poll is the baseline: what a process had before the poll began is not news.
    w.poll(r);
    CHECK(g->counts().empty());
    w.poll(r);
    CHECK(g->counts().empty());
    // Each counter that moved: once an occurrence for the abandonments and the exceptions...
    r.sourceThreadsAbandoned = 6;
    w.poll(r);
    CHECK(g->counts() == (Counts{{"recovered.srcthread", 1}}));
    r.vendorCallsAbandoned = 9;
    r.dspThreadExceptions = 3;
    w.poll(r);
    CHECK(g->counts() == (Counts{{"recovered.srcthread", 1}, {"recovered.vendorcall", 2}, {"recovered.dspexc", 1}}));
    // ...once a session for the ring, however much more it drops, and however often it is polled.
    r.ringDroppedSamples = 4096;
    w.poll(r);
    r.ringDroppedSamples = 1 << 20;
    w.poll(r);
    w.poll(r);
    CHECK(countOf(g->counts(), "recovered.ringdrop") == 1);
    // A counter that went DOWN (a new process-wide baseline in a test) is not a count.
    health::RecoveryReadings lower;
    w.poll(lower);
    CHECK(g->counts().size() == 4);
    // Nothing is counted into a ledger that is off.
    g->reset();
    g->disarm();
    health::RecoveryWatch off;
    off.poll(health::RecoveryReadings{});
    health::RecoveryReadings moved;
    moved.sourceThreadsAbandoned = 3;
    off.poll(moved);
    CHECK(g->counts().empty());
    g->reset();
}

void testARaisedFlagIsCountedWhereTheDrainRuns() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
    // The raise is for a thread that must not take a lock: it counts nothing by itself.
    std::thread t([] {
        for (int i = 0; i < 1000; ++i) { health::raiseRecovered(health::Recovered::PluginApi); }
    });
    t.join();
    CHECK(g->counts().empty());
    health::drainRecovered();
    CHECK(g->counts() == (Counts{{"recovered.pluginapi", 1}}));
    health::drainRecovered();                       // nothing new raised: nothing counted
    CHECK(g->counts() == (Counts{{"recovered.pluginapi", 1}}));
    // A word that is counted per occurrence: one count per drain that finds it raised.
    health::raiseRecovered(health::Recovered::DspExc);
    health::drainRecovered();
    health::raiseRecovered(health::Recovered::DspExc);
    health::drainRecovered();
    CHECK(countOf(g->counts(), "recovered.dspexc") == 2);
    // noteRecovered counts at once, and refuses nothing it should count.
    health::noteRecovered(health::Recovered::CfgSave);
    health::noteRecovered(health::Recovered::CfgSave);
    CHECK(countOf(g->counts(), "recovered.cfgsave") == 1);
    health::noteRecovered(health::Recovered::PatchLoad);
    health::noteRecovered(health::Recovered::PatchLoad);
    CHECK(countOf(g->counts(), "recovered.patchload") == 2);
    g->reset();
}

// The cases the Worker's tests read too (telemetry-worker/test-fixtures/health-cases.json):
// one text in, the canonical text out, by both implementations.
void testTheSharedSelectionCases() {
    const nlohmann::json cases =
        nlohmann::json::parse(sourceFile("telemetry-worker/test-fixtures/health-cases.json"), nullptr, false);
    CHECK(cases.is_array());
    if (!cases.is_array()) { return; }
    CHECK(cases.size() >= 12);
    for (const nlohmann::json& c : cases) {
        const std::string name = c.value("name", std::string());
        const std::string got = health::sanitise(c["input"].get<std::string>());
        const std::string want = c["expected"].get<std::string>();
        if (got != want) {
            std::printf("FAIL case \"%s\":\n   input    %s\n   got      %s\n   expected %s\n", name.c_str(),
                        c["input"].get<std::string>().c_str(), got.c_str(), want.c_str());
        }
        CHECK(got == want);
        // and what it expects is a text encode() itself writes.
        Counts back;
        CHECK(health::decode(want, back));
        CHECK(health::encode(back) == want);
    }
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
        {"hydrasdr", "no HydraSDR found (is it plugged in, and bound to WinUSB?)", RadioReason::Bind},
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
        // THE RTL_TCP CLIENT'S OWN SENTENCES (rtl_tcp_source.cpp, connectLocked, and the shared
        // socket code's connect failures), word for word. A refusal and an address that speaks
        // something else are "nothing there"; a connect or a header read that ran out of time
        // is a timeout, which is checked first so the "nothing at" in the same sentence does
        // not win.
        {"rtltcp",
         "could not reach the rtl_tcp server at the host:1234 - connect failed (network error "
         "10061) (nothing at that address accepted the connection - is rtl_tcp running there?)",
         RadioReason::Absent},
        {"rtltcp",
         "could not find the host \"the-host\" on the network (nothing at that name was found - "
         "is rtl_tcp running there, and is the address right?)",
         RadioReason::Absent},
        {"rtltcp",
         "nothing at the-host:1234 speaks rtl_tcp (it answered, but not with an rtl_tcp header)",
         RadioReason::Absent},
        {"rtltcp",
         "could not reach the rtl_tcp server at the-host:1234 - the rtl_tcp server did not "
         "answer in time (is it at this address?) (nothing at that address accepted the "
         "connection - is rtl_tcp running there?)",
         RadioReason::Timeout},
        {"rtltcp",
         "nothing at the-host:1234 answered as an rtl_tcp server (the rtl_tcp server stopped "
         "answering (receive timed out))",
         RadioReason::Timeout},
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
std::vector<std::pair<std::string, std::string>> docRows(const std::string& doc, const std::string& heading);

std::vector<std::string> docWords(const std::string& doc, const std::string& heading) {
    std::vector<std::string> words;
    for (const auto& row : docRows(doc, heading)) { words.push_back(row.first); }
    return words;
}

// The same table as (first code-span word, whole row) pairs.
std::vector<std::pair<std::string, std::string>> docRows(const std::string& doc, const std::string& heading) {
    std::vector<std::pair<std::string, std::string>> words;
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
        words.push_back({line.substr(a + 1, b - a - 1), line});
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
    std::vector<std::string> slowScopes, slowTiers, recovered;
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
        if (e.name == "slow") { l.slowScopes = e.qualifiers[0]; l.slowTiers = e.qualifiers[1]; }
        if (e.name == "recovered") { l.recovered = e.qualifiers[0]; }
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
    same("Slow-frame scopes", code.slowScopes);
    same("Slow-frame tiers", code.slowTiers);
    same("Recovered from", code.recovered);
    // `user-wait` is named in the page - as the one scope that is NOT counted - but is never a
    // word of the scopes table.
    CHECK(doc.find("`user-wait`") != std::string::npos);
    CHECK(std::find(code.slowScopes.begin(), code.slowScopes.end(), "user-wait") == code.slowScopes.end());
    // The "Recovered from" table says "Once a session." on exactly the rows the code counts once a session.
    for (const auto& row : docRows(doc, "Recovered from")) {
        bool codeOnce = false;
        for (std::size_t i = 0; i < health::recoveredWords().size(); ++i) {
            if (health::recoveredWords()[i] == row.first) {
                codeOnce = health::recoveredOncePerSession(static_cast<health::Recovered>(i));
            }
        }
        const bool docOnce = row.second.find("Once a session.") != std::string::npos;
        if (docOnce != codeOnce) { std::printf("FAIL PRIVACY.md row for %s: once a session %d, code %d\n", row.first.c_str(), docOnce, codeOnce); }
        CHECK(docOnce == codeOnce);
    }
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
    CHECK(constant("MAX_HEALTH_BASE_TOKENS") == static_cast<long>(health::kMaxBaseTokens));
    CHECK(constant("MAX_HEALTH_SLOW_TOKENS") == static_cast<long>(health::kMaxSlowTokens));
    CHECK(constant("MAX_HEALTH_RECOVERED_TOKENS") == static_cast<long>(health::kMaxRecoveredTokens));
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
    testSlowFrameWordsAreTheFrameTimersOwn();
    testSlowFramesCountThroughTheLedger();
    testSlowTokensPerRecordKeepTheWorst();
    testRecoveredWordsAndCap();
    testARecordCarriesTheSelectionAndSettlesExactlyThat();
    testTheLedgersFileHoldsEverythingItMayCarry();
    testASlowFrameNeverWaitsOnTheDisk();
    testTheWatchCountsWhatTheCountersSay();
    testARaisedFlagIsCountedWhereTheDrainRuns();
    testTheSharedSelectionCases();
    testReasonsFromTheDriversOwnSentences();
    testTheTranslatedSentencesAreClassifiedInTheLanguageInForce();
    testTheRecordCarriesOnlyVocabulary();
    testPrivacyListsEveryWordOfTheVocabulary();
    testTheWorkerCarriesTheSameVocabulary();
    return testSummary("test_health_events");
}
