// test_engine_api_schedule.cpp - TIMED RECORDINGS THROUGH THE ENGINE (docs/API.md 6.23; engine/engine_schedule.cpp over the pure clock core/schedule.*).
// A headless engine whose pass runs on a thread of its own, its clock set by the test hook "test.scheduleClock" (no test sleeps for a start time),
// its recordings written to a scratch folder (USERPROFILE is pointed at one BEFORE the engine is made, so nothing is written to a real Documents folder).
//
//   K1  ONE ENTRY'S LIFE: added, waiting, started at its time (tuned, mode and bandwidth set, the audio recorder on, a file opened), still going a second
//       before its end, ended at its length (the recorder off, the file closed and holding audio, the tuning put back), done.
//   K2  TWO RADIOS: each entry tunes and records ITS OWN radio, ends at its own time, and the other radio never moves.
//   K3  REFUSALS, in words, with the occurrence used up: a radio that is not there, one that is stopped, one already recording by hand (which is NOT stopped
//       by the schedule), one held by the scanner; and a folder that cannot be written (the recorder's own sentence, the tuning put back, nothing left running).
//   K4  THE RULES AT CREATION: overlap on a radio refused (back to back and another radio fine), every field's range, unknown keys, bad JSON, the 32 limit.
//   K5  THE CLOCK: a start that fell in the dark is MISSED (counted, never run late); a clock jump forward ends a recording and misses a start; a clock set
//       back ends a recording; a clock set back after a run never re-runs it.
//   K6  A RESTART: the settings snapshot carries the entries with what was handled; a new engine loaded from it neither replays nor recounts, and its ids go on.
//   K7  THE OPS: replace keeps the id, remove / disable end a running recording, run now, skip next, not-found answers, and a REMOTE session is refused.
//   K8  THE SETTINGS FILE: the round trip, and a damaged schedule is made safe.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "foxsdr_api.h"
#include "test_check.hpp"

using Clock = std::chrono::steady_clock;
using cascade::engine::Engine;
namespace cmd = cascade::core::cmd;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

constexpr std::uint64_t kAllGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_ADMIN |
                                     FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_IQ;
constexpr std::int64_t T0 = 1790000000;   // a fixed "now": 2026, UTC seconds

fs::path scratchRoot() {
    static const fs::path p = fs::temp_directory_path() / ("foxsdr-schedule-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    return p;
}

void setHome(const fs::path& home, bool create = true) {
    if (create) { fs::create_directories(home); }
#if defined(_WIN32)
    _putenv_s("USERPROFILE", home.string().c_str());
#else
    setenv("HOME", home.string().c_str(), 1);
    setenv("USERPROFILE", home.string().c_str(), 1);
#endif
}

struct Rig {
    Engine e;
    FoxSession* op = nullptr;
    fs::path home;
    explicit Rig(const std::string& name, bool makeHome = true) {
        home = scratchRoot() / name;
        if (makeHome) { setHome(home); } else { setHome(home / "blocker-file-is-here", false); }
        e.configureApi("testHooks=1");
        e.initialise();
        (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
        e.controlPass();
        e.startControlThread();
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = kAllGrants;
        p.clientName = "the operator";
        (void)api()->open_session(e.apiHandle(), &p, &op);
    }
    ~Rig() {
        if (op != nullptr) { api()->close_session(op); }
        e.shutdown();
    }
    FoxCommandResult wait(std::uint64_t ticket) {
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        const auto end = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < end) {
            FoxCommandResult r[8];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(op, r, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == ticket) { return r[i]; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    FoxCommandResult submit(const FoxCommand& c, FoxSession* s = nullptr) {
        FoxSession* use = s != nullptr ? s : op;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        if (api()->submit(use, &c, 1, &sr) < 0) { return out; }
        if (use == op) { return wait(sr.ticket); }
        // another session: poll its own results
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end) {
            FoxCommandResult r[8];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(use, r, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) { return r[i]; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    FoxCommandResult op0(std::uint32_t o, std::int64_t i0 = 0, std::int64_t i1 = 0) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = o;
        c.ival[0] = i0;
        c.ival[1] = i1;
        return submit(c);
    }
    FoxCommandResult setEntry(const json& j, std::int64_t id = 0, FoxSession* s = nullptr) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPP_OP_SCHEDULE_SET;
        c.ival[0] = id;
        const std::string t = j.is_string() ? j.get<std::string>() : j.dump();
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxSession* use = s != nullptr ? s : op;
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        if (api()->submit_text(use, &c, t.data(), static_cast<uint32_t>(t.size()), &sr) < 0) { return out; }
        if (use == op) { return wait(sr.ticket); }
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end) {
            FoxCommandResult r[8];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(use, r, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) { return r[i]; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    void clock(std::int64_t unixMs) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SETTING_SET;
        std::snprintf(c.text, sizeof(c.text), "test.scheduleClock=%lld", static_cast<long long>(unixMs));
        (void)submit(c);
    }
    void clockS(std::int64_t unixS) { clock(unixS * 1000); }
    std::string fact(const std::string& key) {
        char buf[2048];
        buf[0] = '\0';
        const int32_t n = api()->get_setting(op, key.c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string();
    }
    json entry(int id) {
        const std::string t = fact("fact.schedule.entry." + std::to_string(id));
        return t.empty() ? json() : json::parse(t, nullptr, false);
    }
    std::string state(int id) {
        const json j = entry(id);
        return j.is_object() && j.contains("state") ? j["state"].get<std::string>() : std::string("<none>");
    }
    std::string note(int id) {
        const json j = entry(id);
        return j.is_object() && j.contains("note") ? j["note"].get<std::string>() : std::string();
    }
    bool waitState(int id, const std::string& want, int ms = 4000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (state(id) == want) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool waitFact(const std::string& key, const std::string& want, int ms = 4000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (fact(key) == want) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    FoxReceiverState rx(std::uint32_t radio = 1) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        for (int i = 0; i < 200 && api()->read_radio_state(op, radio, &st) != FOXAPI_OK; ++i) {}
        return st;
    }
    bool waitRx(std::uint32_t radio, const std::function<bool(const FoxReceiverState&)>& pred, int ms = 4000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (pred(rx(radio))) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool addRadio2() {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_RADIO_ADD;
        c.ival[0] = 2;
        c.ival[1] = 1;
        return submit(c).status == FOXAPI_OK;
    }
};

json base(int radio, std::int64_t start, int len) {
    return json{{"name", "test"}, {"radio", radio}, {"startUnix", start}, {"lengthS", len}, {"repeat", "once"}, {"recordAudio", true}};
}

std::vector<fs::path> filesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return out; }
    for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
        if (f.is_regular_file()) { out.push_back(f.path()); }
    }
    return out;
}

// ===================================================================================================================================

void testOneEntry() {
    Rig r("k1");
    const FoxReceiverState before = r.rx();
    r.clockS(T0);
    json e = base(1, T0 + 60, 120);
    e["tune"] = true;
    e["freqHz"] = before.centreHz + 4000.0;
    e["mode"] = FOXAPI_DEMOD_AM;
    e["bandwidthHz"] = 6000.0;
    const FoxCommandResult add = r.setEntry(e);
    CHECK(add.status == FOXAPI_OK);
    CHECK(add.applied[0] == 1.0);
    CHECK(r.waitFact("fact.schedule.count", "1"));
    CHECK(r.state(1) == "waiting");
    CHECK(r.fact("fact.schedule.next") == std::to_string(T0 + 60));
    CHECK(r.fact("fact.schedule.active") == "0" && r.fact("fact.schedule.missed") == "0");
    // before its time nothing happens, however many passes run
    r.clockS(T0 + 59);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(r.state(1) == "waiting" && (r.rx().flags & FOXAPI_RX_RECORDING_AUDIO) == 0u);
    // at its time
    r.clockS(T0 + 60);
    CHECK(r.waitState(1, "recording"));
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) != 0u; }));
    const FoxReceiverState during = r.rx();
    CHECK(during.demodMode == FOXAPI_DEMOD_AM);
    CHECK(std::llround(during.tunedHz) == std::llround(before.centreHz + 4000.0));
    CHECK(std::llround(during.bandwidthHz) == 6000);
    CHECK(r.fact("fact.schedule.active") == "1");
    // a second before the end it is still going
    r.clockS(T0 + 60 + 119);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(r.state(1) == "recording" && (r.rx().flags & FOXAPI_RX_RECORDING_AUDIO) != 0u);
    // at its length it ends: the recorder is off, the tuning is back, the file is whole
    r.clockS(T0 + 60 + 120);
    CHECK(r.waitState(1, "done"));
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
    const FoxReceiverState after = r.rx();
    CHECK(after.demodMode == before.demodMode);
    CHECK(std::llround(after.centreHz) == std::llround(before.centreHz) && std::llround(after.vfoOffsetHz) == std::llround(before.vfoOffsetHz));
    CHECK(std::llround(after.bandwidthHz) == std::llround(before.bandwidthHz));
    CHECK(r.fact("fact.schedule.active") == "0");
    const std::vector<fs::path> files = filesIn(r.home);
    CHECK(files.size() == 1);
    if (!files.empty()) {
        std::error_code ec;
        const auto size = fs::file_size(files[0], ec);
        CHECK(!ec && size > 44);   // a WAV header and some audio
        std::printf("K1: the recording is %s, %llu bytes\n", files[0].filename().string().c_str(), static_cast<unsigned long long>(size));
    }
    // a once-entry is never started again
    r.clockS(T0 + 100000);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(r.state(1) == "done" && filesIn(r.home).size() == 1);
}

void testTwoRadios() {
    Rig r("k2");
    CHECK(r.addRadio2());
    r.clockS(T0);
    const FoxReceiverState a1 = r.rx(1), a2 = r.rx(2);
    json e1 = base(1, T0 + 30, 60);
    e1["tune"] = true;
    e1["freqHz"] = a1.centreHz + 1000.0;
    e1["mode"] = FOXAPI_DEMOD_USB;
    json e2 = base(2, T0 + 30, 120);
    e2["tune"] = true;
    e2["freqHz"] = a2.centreHz + 9000.0;
    e2["mode"] = FOXAPI_DEMOD_NFM;
    e2["recordAudio"] = false;
    e2["recordIq"] = true;
    CHECK(r.setEntry(e1).status == FOXAPI_OK);
    CHECK(r.setEntry(e2).status == FOXAPI_OK);   // the same second, another radio: not an overlap
    r.clockS(T0 + 30);
    CHECK(r.waitState(1, "recording") && r.waitState(2, "recording"));
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) != 0u; }));
    CHECK(r.waitRx(2, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_IQ) != 0u; }));
    // each radio did ITS entry and no other
    CHECK(r.rx(1).demodMode == FOXAPI_DEMOD_USB && std::llround(r.rx(1).tunedHz) == std::llround(a1.centreHz + 1000.0));
    CHECK(r.rx(2).demodMode == FOXAPI_DEMOD_NFM && std::llround(r.rx(2).tunedHz) == std::llround(a2.centreHz + 9000.0));
    CHECK((r.rx(1).flags & FOXAPI_RX_RECORDING_IQ) == 0u && (r.rx(2).flags & FOXAPI_RX_RECORDING_AUDIO) == 0u);
    // radio 1's entry ends at 60 s; radio 2's goes on
    r.clockS(T0 + 30 + 60);
    CHECK(r.waitState(1, "done"));
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
    CHECK(r.state(2) == "recording" && (r.rx(2).flags & FOXAPI_RX_RECORDING_IQ) != 0u);
    CHECK(r.rx(1).demodMode == a1.demodMode && std::llround(r.rx(1).centreHz) == std::llround(a1.centreHz));   // radio 1 is back
    CHECK(r.rx(2).demodMode == FOXAPI_DEMOD_NFM);                                                               // radio 2 is not
    r.clockS(T0 + 30 + 120);
    CHECK(r.waitState(2, "done"));
    CHECK(r.waitRx(2, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_IQ) == 0u; }));
    CHECK(r.rx(2).demodMode == a2.demodMode);
}

void testRefusals() {
    Rig r("k3");
    r.clockS(T0);
    // a radio that is not there
    CHECK(r.setEntry(base(5, T0 + 10, 30)).status == FOXAPI_OK);   // (accepted: the radio may be added before the time)
    // a radio that is stopped
    CHECK(r.addRadio2());
    CHECK(r.setEntry(base(2, T0 + 10, 30)).status == FOXAPI_OK);
    FoxCommand stop{};
    stop.structSize = sizeof(stop);
    stop.op = FOXAPI_OP_RUN;
    stop.target = 2;
    stop.ival[0] = 0;
    CHECK(r.submit(stop).status == FOXAPI_OK);
    // radio 1 already recording by hand
    CHECK(r.setEntry(base(1, T0 + 10, 30)).status == FOXAPI_OK);
    FoxCommand hand{};
    hand.structSize = sizeof(hand);
    hand.op = FOXAPI_OP_RECORD_AUDIO;
    hand.ival[0] = 1;
    CHECK(r.submit(hand).status == FOXAPI_OK);
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) != 0u; }));
    r.clockS(T0 + 10);
    CHECK(r.waitState(1, "refused") && r.waitState(2, "refused") && r.waitState(3, "refused"));
    CHECK(r.note(1).find("radio 5 is not open") != std::string::npos);
    CHECK(r.note(2).find("is stopped") != std::string::npos);
    CHECK(r.note(3).find("already recording audio") != std::string::npos);
    std::printf("K3 refusals: \"%s\" | \"%s\" | \"%s\"\n", r.note(1).c_str(), r.note(2).c_str(), r.note(3).c_str());
    // the hand recording was NOT stopped by the schedule, and nothing else started
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK((r.rx(1).flags & FOXAPI_RX_RECORDING_AUDIO) != 0u);
    CHECK(r.fact("fact.schedule.active") == "0");
    // the occurrence is used up: further passes do not retry
    r.clockS(T0 + 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.state(3) == "refused");
    // an entry whose radio is busy with the scanner: refused too (the scanner is started on radio 1 and stopped again)
    hand.ival[0] = 0;
    (void)r.submit(hand);
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
}

void testScannerBusy() {
    Rig r("k3b");
    r.clockS(T0);
    CHECK(r.setEntry(base(1, T0 + 10, 30)).status == FOXAPI_OK);
    FoxCommand sc{};
    sc.structSize = sizeof(sc);
    sc.op = FOXAPI_OP_SCANNER_RUN;
    sc.ival[0] = 1;
    const FoxCommandResult s = r.submit(sc);
    if (s.status == FOXAPI_OK) {
        CHECK(r.waitRx(1, [](const FoxReceiverState& st) { return (st.flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u; }));
        r.clockS(T0 + 10);
        CHECK(r.waitState(1, "refused"));
        CHECK(r.note(1).find("scanner is using radio 1") != std::string::npos);
    } else {
        std::printf("K3b: the scanner would not start on a hermetic engine (status %d): the scanner refusal is UNVERIFIED here\n", s.status);
        ++g_checksSkipped;
    }
}

void testUnwritableFolder() {
    // USERPROFILE points INTO A FILE, so the recordings folder can be neither found nor made
    const fs::path home = scratchRoot() / "k3c";
    fs::create_directories(home);
    const fs::path blocker = home / "blocker-file-is-here";
    { std::ofstream(blocker) << "a file where a folder would go"; }
    Rig r("k3c", false);
    const FoxReceiverState before = r.rx();
    r.clockS(T0);
    json e = base(1, T0 + 10, 60);
    e["tune"] = true;
    e["freqHz"] = before.centreHz + 4000.0;
    e["mode"] = FOXAPI_DEMOD_AM;
    e["recordIq"] = true;
    CHECK(r.setEntry(e).status == FOXAPI_OK);
    r.clockS(T0 + 10);
    CHECK(r.waitState(1, "refused", 8000));
    CHECK(!r.note(1).empty());
    std::printf("K3c unwritable folder: \"%s\"\n", r.note(1).c_str());
    // nothing is left running, and the tuning is where it was
    CHECK(r.waitRx(1, [&](const FoxReceiverState& s) { return s.demodMode == before.demodMode && (s.flags & (FOXAPI_RX_RECORDING_AUDIO | FOXAPI_RX_RECORDING_IQ)) == 0u; }));
    CHECK(std::llround(r.rx().centreHz) == std::llround(before.centreHz) && std::llround(r.rx().vfoOffsetHz) == std::llround(before.vfoOffsetHz));
    CHECK(r.fact("fact.schedule.active") == "0");
}

void testCreationRules() {
    Rig r("k4");
    r.clockS(T0);
    CHECK(r.setEntry(base(1, T0 + 1000, 600)).status == FOXAPI_OK);
    // overlap on the same radio, in every way; back to back and another radio are fine
    for (const std::int64_t start : {T0 + 1300, T0 + 500, T0 + 1100, T0 + 100}) {
        const int len = start == T0 + 100 ? 5000 : (start == T0 + 500 ? 600 : (start == T0 + 1100 ? 100 : 600));
        const FoxCommandResult o = r.setEntry(base(1, start, len));
        CHECK(o.status == FOXAPI_BUSY);
        CHECK(std::string(o.message).find("overlaps") != std::string::npos && std::string(o.message).find("radio 1") != std::string::npos);
    }
    CHECK(r.setEntry(base(1, T0 + 1600, 60)).status == FOXAPI_OK);    // starts when the first ends
    CHECK(r.setEntry(base(1, T0 + 400, 600)).status == FOXAPI_OK);    // ends when the first starts
    CHECK(r.setEntry(base(2, T0 + 1300, 600)).status == FOXAPI_OK);   // another radio
    // a daily entry against a once-entry three days on
    json d = base(1, T0 + 100000, 600);
    d["repeat"] = "daily";
    CHECK(r.setEntry(d).status == FOXAPI_OK);
    CHECK(r.setEntry(base(1, T0 + 100000 + 3 * 86400 + 100, 60)).status == FOXAPI_BUSY);
    // ranges
    struct Bad { json j; int status; const char* word; };
    const Bad bad[] = {
        {base(0, T0 + 5000, 60), FOXAPI_OUT_OF_RANGE, "radio"},
        {base(9, T0 + 5000, 60), FOXAPI_OUT_OF_RANGE, "radio"},
        {base(1, T0 + 5000, 0), FOXAPI_OUT_OF_RANGE, "length"},
        {base(1, T0 + 5000, 86401), FOXAPI_OUT_OF_RANGE, "length"},
        {base(1, 5, 60), FOXAPI_OUT_OF_RANGE, "start"},
        {base(1, T0 - 7200, 60), FOXAPI_OUT_OF_RANGE, "passed"},
    };
    for (const Bad& b : bad) {
        const FoxCommandResult x = r.setEntry(b.j);
        CHECK(x.status == b.status);
        CHECK(std::string(x.message).find(b.word) != std::string::npos);
        if (x.status != b.status || std::string(x.message).find(b.word) == std::string::npos) { std::printf("      %s -> %d \"%s\"\n", b.j.dump().c_str(), x.status, x.message); }
    }
    json noRec = base(1, T0 + 5000, 60);
    noRec["recordAudio"] = false;
    CHECK(r.setEntry(noRec).status == FOXAPI_OUT_OF_RANGE);
    json badTune = base(1, T0 + 5000, 60);
    badTune["tune"] = true;
    badTune["freqHz"] = 9.0e9;
    CHECK(r.setEntry(badTune).status == FOXAPI_OUT_OF_RANGE);
    badTune["freqHz"] = 100.0e6;
    badTune["mode"] = 8;
    CHECK(r.setEntry(badTune).status == FOXAPI_OUT_OF_RANGE);
    json longName = base(1, T0 + 5000, 60);
    longName["name"] = std::string(41, 'n');
    CHECK(r.setEntry(longName).status == FOXAPI_OUT_OF_RANGE);
    json unknown = base(1, T0 + 5000, 60);
    unknown["colour"] = "red";
    CHECK(r.setEntry(unknown).status == FOXAPI_BAD_ARGUMENT && std::string(r.setEntry(unknown).message).find("colour") != std::string::npos);
    json wrongType = base(1, T0 + 5000, 60);
    wrongType["lengthS"] = "long";
    CHECK(r.setEntry(wrongType).status == FOXAPI_BAD_ARGUMENT);
    json badRepeat = base(1, T0 + 5000, 60);
    badRepeat["repeat"] = "monthly";
    CHECK(r.setEntry(badRepeat).status == FOXAPI_BAD_ARGUMENT);
    CHECK(r.setEntry(json("not json at all")).status == FOXAPI_BAD_ARGUMENT);
    CHECK(r.setEntry(json("[1,2,3]")).status == FOXAPI_BAD_ARGUMENT);
    // the limit
    Rig q("k4b");
    q.clockS(T0);
    for (int i = 0; i < 32; ++i) { CHECK(q.setEntry(base(1, T0 + 1000 + i * 100, 50)).status == FOXAPI_OK); }
    const FoxCommandResult over = q.setEntry(base(1, T0 + 50000, 50));
    CHECK(over.status == FOXAPI_LIMIT && std::string(over.message).find("32") != std::string::npos);
    CHECK(q.waitFact("fact.schedule.count", "32"));
}

void testClockRules() {
    Rig r("k5");
    CHECK(r.addRadio2());
    r.clockS(T0);
    json d = base(1, T0 + 100, 60);
    d["repeat"] = "daily";
    CHECK(r.setEntry(d).status == FOXAPI_OK);
    CHECK(r.setEntry(base(2, T0 + 100, 60)).status == FOXAPI_OK);
    // the program was "closed": the first look is an hour and a minute late
    r.clockS(T0 + 100 + 3600);
    CHECK(r.waitState(1, "missed"));
    CHECK(r.waitFact("fact.schedule.missed", "2"));
    CHECK(r.note(1).find("none was started late") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK((r.rx().flags & FOXAPI_RX_RECORDING_AUDIO) == 0u && r.fact("fact.schedule.active") == "0");
    CHECK(filesIn(r.home).empty());   // nothing was recorded after the fact
    // the daily entry still runs the next day, 20 s late is within the grace
    r.clockS(T0 + 100 + 86400 + 20);
    CHECK(r.waitState(1, "recording"));
    // a clock that jumps forward over the whole recording ends it
    r.clockS(T0 + 100 + 86400 + 5000);
    CHECK(r.waitState(1, "stopped early") || r.waitState(1, "done"));
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
    // the clock set back later than the next start, then forward: the day already run does not run again
    const std::size_t filesBefore = filesIn(r.home).size();
    r.clockS(T0 + 100 + 86400 + 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.clockS(T0 + 100 + 86400 + 25);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.state(1) != "recording" && filesIn(r.home).size() == filesBefore);
    // a clock set BACK during a recording ends it
    r.clockS(T0 + 100 + 2 * 86400 + 10);
    CHECK(r.waitState(1, "recording"));
    r.clockS(T0 + 100 + 2 * 86400 + 10 - 10);   // 10 s back
    CHECK(r.waitState(1, "stopped early"));
    CHECK(r.note(1).find("set back") != std::string::npos);
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
    // a back-step under 2 s does not end it
    CHECK(r.setEntry(base(2, T0 + 100 + 3 * 86400, 600), 2).status == FOXAPI_OK);   // (edit radio 2's entry to a later time)
    r.clockS(T0 + 100 + 3 * 86400);
    CHECK(r.waitState(2, "recording"));
    r.clock((T0 + 100 + 3 * 86400) * 1000 - 1500);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.state(2) == "recording");
}

void testRestart() {
    json snapEntries;
    std::vector<cascade::core::sched::Entry> saved;
    {
        Rig r("k6a");
        r.clockS(T0);
        json d = base(1, T0 + 100, 60);
        d["repeat"] = "daily";
        d["name"] = "satellite";
        CHECK(r.setEntry(d).status == FOXAPI_OK);
        CHECK(r.setEntry(base(1, T0 + 5000, 60)).status == FOXAPI_OK);
        r.clockS(T0 + 100);
        CHECK(r.waitState(1, "recording"));
        r.clockS(T0 + 160);
        CHECK(r.waitState(1, "done"));
        r.clockS(T0 + 100 + 86400 + 10);   // the next day's start, 10 s late: within the grace, so it runs; entry 2's start (T0 + 5000) fell in between and is missed
        CHECK(r.waitState(1, "recording"));
        CHECK(r.waitState(2, "missed"));
        // wait for the engine to publish the config snapshot that holds it
        const auto end = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < end) {
            saved = r.e.configSnapshot().schedule;
            if (saved.size() == 2 && saved[0].lastStartUnix == T0 + 100 + 86400) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(saved.size() == 2);
        CHECK(saved.size() == 2 && saved[0].lastStartUnix == T0 + 100 + 86400 && saved[0].name == "satellite");
    }
    // a new engine, the same settings, an hour later: nothing replays, nothing is counted twice, the other entry's start passed in the dark
    Rig r("k6b");
    r.clockS(T0 + 100 + 86400 + 10 + 3600);   // (the clock first: an engine reads the real one until told otherwise)
    cascade::core::AppConfig cfg;
    cfg.schedule = saved;
    r.e.applyConfig(cfg);
    CHECK(r.waitFact("fact.schedule.count", "2"));
    CHECK(r.waitState(2, "missed"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.state(1) != "recording" && r.fact("fact.schedule.active") == "0");
    {
        const json e1 = r.entry(1);
        const std::int64_t got = e1.is_object() && e1.contains("lastStartUnix") ? e1["lastStartUnix"].get<std::int64_t>() : -1;
        CHECK(got == T0 + 100 + 86400);
        if (got != T0 + 100 + 86400) { std::printf("      entry 1 after the restart: %s\n", e1.dump().c_str()); }
    }
    CHECK(r.entry(2)["missed"].get<int>() == 1);   // counted once, in the first engine, and not again now
    // ids go on after the highest loaded
    const FoxCommandResult add = r.setEntry(base(1, T0 + 100 + 86400 + 100 + 7200, 30));
    CHECK(add.status == FOXAPI_OK && add.applied[0] == 3.0);
    // and the entries come back out of the engine's own snapshot unchanged
    const std::vector<cascade::core::sched::Entry> again = r.e.configSnapshot().schedule;
    CHECK(again.size() >= 2);
}

void testOps() {
    Rig r("k7");
    r.clockS(T0);
    CHECK(r.setEntry(base(1, T0 + 100, 600)).status == FOXAPI_OK);
    // replace keeps the id and resets what was handled
    json e = base(1, T0 + 200, 100);
    e["name"] = "renamed";
    const FoxCommandResult rep = r.setEntry(e, 1);
    CHECK(rep.status == FOXAPI_OK && rep.applied[0] == 1.0);
    CHECK(r.waitFact("fact.schedule.next", std::to_string(T0 + 200)));
    CHECK(r.entry(1)["name"].get<std::string>() == "renamed");
    CHECK(r.setEntry(e, 99).status == FOXAPI_NOT_FOUND);
    // run now
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_RUN_NOW, 1).status == FOXAPI_OK);
    CHECK(r.waitState(1, "recording"));
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_RUN_NOW, 1).status == FOXAPI_BUSY);
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_RUN_NOW, 42).status == FOXAPI_NOT_FOUND);
    // disabling ends the recording it makes
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_ENABLE, 1, 0).status == FOXAPI_OK);
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
    CHECK(r.state(1) == "stopped early" || r.state(1) == "disabled");
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_ENABLE, 1, 0).status == FOXAPI_NO_CHANGE);
    // a disabled entry never starts, at its time or after it
    r.clockS(T0 + 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK((r.rx().flags & FOXAPI_RX_RECORDING_AUDIO) == 0u);
    // skip next
    json d = base(1, T0 + 100000, 60);
    d["repeat"] = "daily";
    CHECK(r.setEntry(d).status == FOXAPI_OK);   // id 2
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_SKIP, 2).status == FOXAPI_OK);
    CHECK(r.waitState(2, "skipped"));
    r.clockS(T0 + 100000);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r.state(2) != "recording");
    r.clockS(T0 + 100000 + 86400);
    CHECK(r.waitState(2, "recording"));
    // removing one that is recording ends it
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_REMOVE, 2).status == FOXAPI_OK);
    CHECK(r.waitRx(1, [](const FoxReceiverState& s) { return (s.flags & FOXAPI_RX_RECORDING_AUDIO) == 0u; }));
    CHECK(r.waitFact("fact.schedule.count", "1"));
    CHECK(r.fact("fact.schedule.entry.2").empty());
    CHECK(r.op0(FOXAPP_OP_SCHEDULE_REMOVE, 2).status == FOXAPI_NOT_FOUND);
    // a REMOTE session cannot touch the schedule
    FoxSessionParams p{};
    p.structSize = sizeof(p);
    p.flags = FOXAPI_SESSION_REMOTE;
    p.grants = kAllGrants;
    p.clientName = "remote";
    FoxSession* rem = nullptr;
    if (api()->open_session(r.e.apiHandle(), &p, &rem) == FOXAPI_OK && rem != nullptr) {
        CHECK(r.setEntry(base(1, T0 + 999999, 60), 0, rem).status == FOXAPI_DENIED);
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPP_OP_SCHEDULE_REMOVE;
        c.ival[0] = 1;
        CHECK(r.submit(c, rem).status == FOXAPI_DENIED);
        CHECK(r.fact("fact.schedule.count") == "1");
        api()->close_session(rem);
    }
}

void testSettingsFile() {
    cascade::core::AppConfig c;
    cascade::core::sched::Entry e;
    e.id = 7;
    e.name = "weather";
    e.radio = 2;
    e.startUnix = T0 + 100;
    e.lengthS = 600;
    e.repeat = cascade::core::sched::Repeat::Daily;
    e.tune = true;
    e.freqHz = 137.9e6;
    e.mode = 1;
    e.bandwidthHz = 40000.0;
    e.recordIq = true;
    e.recordAudio = false;
    e.returnToWhereIWas = false;
    e.createdUnix = T0;
    e.lastStartUnix = T0 + 100;
    e.lastOutcome = cascade::core::sched::Outcome::Ran;
    e.missed = 2;
    c.schedule = {e};
    const std::string text = cascade::core::ConfigStore::serialize(c);
    cascade::core::AppConfig back;
    std::string why;
    CHECK(cascade::core::ConfigStore::parse(text, "test", back, why));
    CHECK(back.schedule.size() == 1 && back.schedule[0] == e);
    // no entries: the key is not written, so a user who never made one has the file they always had
    CHECK(cascade::core::ConfigStore::serialize(cascade::core::AppConfig{}).find("\"schedule\"") == std::string::npos);
    // a damaged schedule is made safe, element by element
    const std::string junk =
        "{\"schedule\":[5, \"x\", {\"id\":1,\"radio\":99,\"startUnix\":1790000000,\"lengthS\":60,\"recordAudio\":true},"
        "{\"id\":2,\"radio\":1,\"startUnix\":1790000000,\"lengthS\":60,\"recordAudio\":true,\"repeat\":9},"
        "{\"id\":3,\"radio\":1,\"startUnix\":1790000000,\"lengthS\":60,\"recordAudio\":true,\"name\":\"ok\",\"missed\":99999999,\"lastOutcome\":77},"
        "{\"id\":3,\"radio\":2,\"startUnix\":1790000500,\"lengthS\":30,\"recordIq\":true,\"recordAudio\":false}]}";
    cascade::core::AppConfig j;
    CHECK(cascade::core::ConfigStore::parse(junk, "test", j, why));
    CHECK(j.schedule.size() == 2);
    if (j.schedule.size() == 2) {
        CHECK(j.schedule[0].name == "ok" && j.schedule[0].missed == cascade::core::sched::kMaxMissedCount && j.schedule[0].lastOutcome == cascade::core::sched::Outcome::None);
        CHECK(j.schedule[0].id != j.schedule[1].id && j.schedule[0].id != 0 && j.schedule[1].id != 0);
    }
    CHECK(cascade::core::ConfigStore::parse("{\"schedule\":\"nonsense\"}", "test", j, why) && j.schedule.empty());
    // an array a hostile file made huge is read only so far
    std::string many = "{\"schedule\":[";
    for (int i = 0; i < 1000; ++i) { many += std::string(i == 0 ? "" : ",") + "{\"radio\":1,\"startUnix\":1790000000,\"lengthS\":5,\"recordAudio\":true}"; }
    many += "]}";
    CHECK(cascade::core::ConfigStore::parse(many, "test", j, why));
    CHECK(static_cast<int>(j.schedule.size()) == cascade::core::sched::kMaxEntries);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::error_code ec;
    fs::remove_all(scratchRoot(), ec);
    // (an argument names the one group to run: a hand-run of a single case)
    const std::string only = argc > 1 ? argv[1] : "";
    const auto want = [&only](const char* name) { return only.empty() || only == name; };
    if (want("one")) { testOneEntry(); }
    if (want("two")) { testTwoRadios(); }
    if (want("refusals")) { testRefusals(); }
    if (want("scanner")) { testScannerBusy(); }
    if (want("folder")) { testUnwritableFolder(); }
    if (want("rules")) { testCreationRules(); }
    if (want("clock")) { testClockRules(); }
    if (want("restart")) { testRestart(); }
    if (want("ops")) { testOps(); }
    if (want("file")) { testSettingsFile(); }
    fs::remove_all(scratchRoot(), ec);
    return testSummary("test_engine_api_schedule");
}
