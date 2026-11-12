// Screen lane C's pure rules (src/slint/lanec_logic.hpp; docs/multi-radio-records/screen-c.md): the saved record, the field parsers, the list lockouts, the scan-mute follower, the scan watcher, quick
// memories, memory recalls, the delta lock (and the API table that enforces it), the bandwidth memory, the clock, the shortcut overlay's rows, the recordings list and the airband channel names.
// No Slint, no engine: the table under the delta lock is a fake that records what reaches it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/airband_data.hpp"
#include "core/app_commands.hpp"
#include "lanec_logic.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace foxsdr::slintlanec;

namespace {

FoxCommand cmd(std::uint32_t op, std::uint32_t target = 0) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.target = target;
    return c;
}

void storeRoundTrip() {
    Store s;
    CHECK(Store::fromJson("").toJson() == Store{}.toJson());            // a missing file is the defaults
    CHECK(Store::fromJson("not json") == Store{});
    CHECK(Store::fromJson("[1,2]") == Store{});
    s.stepLock = true;
    s.deltaMask = 0x05;
    s.bwMemory = true;
    s.bwByMode[FOXAPI_DEMOD_NFM] = 12500.0;
    s.bwByMode[FOXAPI_DEMOD_AM] = 6000.0;
    s.priorityHz = {162.55e6, 121.5e6};
    s.priorityEveryS = 45.0;
    s.muteScan = true;
    s.logScan = true;
    s.clockMode = 2;
    s.quick[0] = {true, 145.5e6, FOXAPI_DEMOD_NFM, 12500.0};
    s.quick[7] = {true, 7.074e6, FOXAPI_DEMOD_USB, 2700.0};
    Memory m;
    m.squelchDb = -70.0;
    m.thresholdDb = -60.0;
    m.offsetKhz = -600.0;
    m.toneHz = 88.5;
    s.memories[memoryKey("GB3XX", 145.6e6)] = m;
    const Store back = Store::fromJson(s.toJson());
    CHECK(back == s);
    // a memory with every field empty is not written
    Store e;
    e.memories["x|1"] = Memory{};
    CHECK(Store::fromJson(e.toJson()).memories.empty());
    // lenient: bad fields keep their defaults, good ones survive, ranges are clamped
    const Store bad = Store::fromJson(R"({"stepLock":"yes","delta":999,"priorityEvery":1,"clock":9,"quick":[{"hz":-5,"mode":3},{"hz":100e6,"mode":99},{"hz":7e6,"mode":3,"bw":6000}],"mem":{"a|1":{"sq":"x"},"b|2":{"sq":-50}},"bwByMode":{"3":6000,"99":1,"x":2}})");
    CHECK(!bad.stepLock);
    CHECK(bad.deltaMask == (999u & 0xFFu));
    CHECK(bad.priorityEveryS == 5.0);
    CHECK(bad.clockMode == 1);
    CHECK(!bad.quick[0].set && !bad.quick[1].set && bad.quick[2].set && bad.quick[2].hz == 7e6 && bad.quick[2].bandwidthHz == 6000.0);
    CHECK(bad.memories.size() == 1 && bad.memories.count("b|2") == 1 && bad.memories.at("b|2").squelchDb == -50.0);
    CHECK(bad.bwByMode.size() == 1 && bad.bwByMode.at(FOXAPI_DEMOD_AM) == 6000.0);
    CHECK(memoryKey("A B", 118.0e6) == "A B|118000000");
}

void fields() {
    const auto p = parsePriorityList("162.550, 162.4;  121.5  bad 0 -3 5e9");
    CHECK(p.size() == 3 && p[0] == 162.55e6 && p[1] == 162.4e6 && p[2] == 121.5e6);
    CHECK(parsePriorityList("").empty());
    CHECK(parsePriorityList(",,;;").empty());
    std::string many;
    for (int i = 1; i <= 30; ++i) { many += std::to_string(100 + i) + " "; }
    CHECK(parsePriorityList(many).size() == 16);                         // capped
    CHECK(priorityText({162.55e6, 121.5e6, 7.074e6}) == "162.55, 121.5, 7.074");
    CHECK(priorityText({}) == "");
    CHECK(parsePriorityEvery("30").value_or(-12345) == 30.0);
    CHECK(parsePriorityEvery("1").value_or(-12345) == 5.0);
    CHECK(parsePriorityEvery("9999").value_or(-12345) == 600.0);
    CHECK(!parsePriorityEvery("abc").has_value());
    CHECK(!parsePriorityEvery("").has_value());
    bool unset = true;
    CHECK(parseDbField("-70", unset).value_or(-12345) == -70.0 && !unset);
    CHECK(parseDbField("-500", unset).value_or(-12345) == -200.0);
    CHECK(parseDbField("99", unset).value_or(-12345) == 20.0);
    CHECK(std::isnan(parseDbField("off", unset).value_or(-12345)) && unset);
    CHECK(std::isnan(parseDbField("", unset).value_or(-12345)) && unset);
    CHECK(!parseDbField("loud", unset).has_value() && !unset);
    CHECK(dbFieldText(kNaN) == "" && dbFieldText(-70.0) == "-70");
    CHECK(parseKhzField("-600").value_or(-12345) == -600.0);
    CHECK(parseKhzField("").value_or(-12345) == 0.0);
    CHECK(!parseKhzField("1e9").has_value());
    CHECK(!parseKhzField("x").has_value());
    CHECK(parseToneField("88.5").value_or(-12345) == 88.5);
    CHECK(parseToneField("0").value_or(-12345) == 0.0);
    CHECK(parseToneField("").value_or(-12345) == 0.0);
    CHECK(!parseToneField("10").has_value());                            // no CTCSS tone below 60 Hz
    CHECK(!parseToneField("400").has_value());
    CHECK(!parseToneField("hello").has_value());
}

void lockouts() {
    ListLockouts l;
    l.add(7, 118.1e6, 1000.0, 600000.0);     // ten minutes
    l.add(9, 118.2e6, 1000.0, 0.0);          // for good
    CHECK(l.size() == 2 && l.has(7) && l.has(9) && !l.has(1));
    CHECK(l.due(2000.0).empty());
    l.add(7, 118.1e6, 2000.0, 600000.0);     // the same row twice refreshes it, never doubles it
    CHECK(l.size() == 2);
    CHECK(l.due(601000.0).empty());          // refreshed: now due at 602000
    const std::vector<int> due = l.due(602000.0);
    CHECK(due.size() == 1 && due[0] == 7);
    CHECK(l.size() == 1 && !l.has(7));       // taken out when due; the permanent one stays
    CHECK(l.due(1e12).empty());
    const std::vector<int> all = l.clear();
    CHECK(all.size() == 1 && all[0] == 9 && l.size() == 0);
    CHECK(l.clear().empty());
}

void muteFollower() {
    ScanMuteFollower f;
    CHECK(!f.update(true, FOXAPI_SCANNER_IDLE, false).has_value());
    CHECK(f.update(true, FOXAPI_SCANNER_SCANNING, false).value_or(-12345) == 1 && f.weMuted());    // a scan steps: mute
    CHECK(!f.update(true, FOXAPI_SCANNER_SCANNING, true).has_value());                    // nothing to say while it goes on
    CHECK(f.update(true, FOXAPI_SCANNER_PAUSED, false).value_or(-12345) == 0 && !f.weMuted());    // a signal: listen
    CHECK(!f.update(true, FOXAPI_SCANNER_HOLDING, false).has_value());
    CHECK(f.update(true, FOXAPI_SCANNER_SCANNING, false).value_or(-12345) == 1);                   // on again
    CHECK(f.update(true, FOXAPI_SCANNER_IDLE, false).value_or(-12345) == 0);                       // the scan ended while muted: unmute
    // a mute the USER set is never touched
    CHECK(!f.update(true, FOXAPI_SCANNER_SCANNING, true).has_value() && !f.weMuted());
    CHECK(!f.update(true, FOXAPI_SCANNER_PAUSED, true).has_value());
    // the option turned off mid-scan lets go
    ScanMuteFollower g;
    CHECK(g.update(true, FOXAPI_SCANNER_SCANNING, false).value_or(-12345) == 1);
    CHECK(g.update(false, FOXAPI_SCANNER_SCANNING, false).value_or(-12345) == 0);
    // disabled: never mutes
    ScanMuteFollower h;
    CHECK(!h.update(false, FOXAPI_SCANNER_SCANNING, false).has_value());
}

void scanWatch() {
    ScanWatcher w;
    CHECK(w.found() == 0 && w.hits().empty() && w.busiest() == nullptr);
    CHECK(w.observe(0, FOXAPI_SCANNER_SCANNING, 118.1e6, 1000).empty());
    CHECK(w.observe(100, FOXAPI_SCANNER_PAUSED, 118.1e6, 1000).empty());                  // a signal found: no line until it ends
    CHECK(w.found() == 1);
    CHECK(w.observe(2100, FOXAPI_SCANNER_HOLDING, 118.1e6, 1002).empty());                // still listening
    const std::string line = w.observe(4100, FOXAPI_SCANNER_SCANNING, 118.2e6, 1004);    // moved on: the hit ends
    CHECK(line == "1970-01-01T00:16:44Z,118100000,4.0");
    w.observe(5000, FOXAPI_SCANNER_PAUSED, 118.1e6, 1005);                               // the same frequency again
    w.observe(6000, FOXAPI_SCANNER_PAUSED, 121.5e6 - 0.0, 1006);                          // (still paused: no new hit)
    w.observe(7000, FOXAPI_SCANNER_SCANNING, 118.3e6, 1007);
    w.observe(8000, FOXAPI_SCANNER_PAUSED, 121.5e6, 1008);
    w.observe(9000, FOXAPI_SCANNER_IDLE, 121.5e6, 1009);                                  // the scan stopped on it
    CHECK(w.found() == 3 && w.hits().size() == 2);
    CHECK(w.busiest() != nullptr && w.busiest()->hz == 118.1e6 && w.busiest()->count == 2);
    CHECK(std::fabs(w.totalHeardMs() - (4000.0 + 2000.0 + 1000.0)) < 1e-9);
    w.reset();
    CHECK(w.found() == 0 && w.hits().empty());
    CHECK(ScanWatcher::csvHeader() == "time_utc,frequency_hz,heard_seconds");
}

void quickAndMemory() {
    QuickMemory q;
    CHECK(recallCommands(q).empty());                                                    // an empty register sends nothing
    CHECK(quickLabel(2, q) == "M3");
    q = {true, 145.5e6, FOXAPI_DEMOD_NFM, 12500.0};
    CHECK(quickLabel(2, q) == "M3  145.500");
    const auto cmds = recallCommands(q);
    CHECK(cmds.size() == 3 && cmds[0].op == FOXAPI_OP_SET_FREQUENCY && cmds[0].num[0] == 145.5e6 && cmds[1].op == FOXAPI_OP_SET_MODE && cmds[1].ival[0] == FOXAPI_DEMOD_NFM &&
          cmds[2].op == FOXAPI_OP_SET_BANDWIDTH && cmds[2].num[0] == 12500.0);   // frequency, then mode, then the width the mode would have reset
    q.bandwidthHz = 0.0;
    CHECK(recallCommands(q).size() == 2);
    Memory m;
    CHECK(memoryRecallCommands(m).empty());
    CHECK(offsetText(145.6e6, m) == "");
    m.squelchDb = -70.0;
    m.offsetKhz = -600.0;
    const auto rc = memoryRecallCommands(m);
    CHECK(rc.size() == 1 && rc[0].op == FOXAPI_OP_SET_SQUELCH && rc[0].num[0] == -70.0);
    CHECK(offsetText(145.6e6, m) == "145.0000");
    m.squelchDb = 500.0;
    CHECK(memoryRecallCommands(m)[0].num[0] == 20.0);                                    // clamped to what the op accepts
    CHECK(m.empty() == false && Memory{}.empty());
}

// ---- the delta lock ----
struct Seen { std::vector<std::uint32_t> ops; std::vector<double> nums; int textCalls = 0; };
Seen g_seen;
int32_t FOXAPI_CALL fakeSubmit(FoxSession*, const FoxCommand* c, uint32_t n, FoxSubmitResult* r) {
    for (uint32_t i = 0; i < n; ++i) {
        g_seen.ops.push_back(c[i].op);
        g_seen.nums.push_back(c[i].num[0]);
        if (r != nullptr) { r[i].structSize = sizeof(FoxSubmitResult); r[i].status = FOXAPI_OK; r[i].ticket = 100 + i; }
    }
    return static_cast<int32_t>(n);
}
int32_t FOXAPI_CALL fakeSubmitText(FoxSession*, const FoxCommand* c, const char*, uint32_t, FoxSubmitResult* r) {
    ++g_seen.textCalls;
    g_seen.ops.push_back(c->op);
    g_seen.nums.push_back(c->num[0]);
    if (r != nullptr) { r->structSize = sizeof(FoxSubmitResult); r->status = FOXAPI_OK; r->ticket = 7; }
    return 1;
}
std::uint32_t g_mask = 0;
std::uint32_t maskNow() { return g_mask; }
int radioOf(const FoxCommand& c) { return c.target != 0 ? static_cast<int>(c.target) : 1; }

void delta() {
    FoxCommand c = cmd(FOXAPP_OP_VFO_TO_ABSOLUTE);
    c.num[0] = 118.1e6;
    CHECK(decideDelta(c) == DeltaVerdict::Converted && c.op == FOXAPI_OP_SET_FREQUENCY && c.num[0] == 118.1e6);
    FoxCommand v = cmd(FOXAPI_OP_SET_VFO_OFFSET);
    CHECK(decideDelta(v) == DeltaVerdict::Refused);
    FoxCommand f = cmd(FOXAPP_OP_SET_VFO_OFFSET_FREE);
    CHECK(decideDelta(f) == DeltaVerdict::Refused);
    FoxCommand m = cmd(FOXAPI_OP_SET_MODE);
    CHECK(decideDelta(m) == DeltaVerdict::Pass && m.op == FOXAPI_OP_SET_MODE);
    FoxCommand s = cmd(FOXAPI_OP_STEP_TUNE);
    CHECK(decideDelta(s) == DeltaVerdict::Pass);

    FoxEngineApi real{};
    real.structSize = sizeof(real);
    real.submit = &fakeSubmit;
    real.submit_text = &fakeSubmitText;
    const FoxEngineApi* api = deltaLockApi(&real, &maskNow, &radioOf);
    CHECK(api != nullptr && api != &real && api->submit != real.submit);
    const long refusals0 = deltaRefusals(), conv0 = deltaConversions();

    // lock off: everything passes untouched
    g_mask = 0;
    FoxCommand batch[3] = {cmd(FOXAPP_OP_VFO_TO_ABSOLUTE), cmd(FOXAPI_OP_SET_VFO_OFFSET), cmd(FOXAPI_OP_SET_MODE)};
    batch[0].num[0] = 100e6;
    FoxSubmitResult res[3]{};
    CHECK(api->submit(nullptr, batch, 3, res) == 3);
    CHECK(g_seen.ops.size() == 3 && g_seen.ops[0] == FOXAPP_OP_VFO_TO_ABSOLUTE && g_seen.ops[1] == FOXAPI_OP_SET_VFO_OFFSET);
    // lock on for radio 1: the click becomes a retune, the offset is refused (DENIED, ticket 0), the mode goes through, and the result slots line up
    g_seen = Seen{};
    g_mask = 1u;
    for (auto& r : res) { r = FoxSubmitResult{}; }
    CHECK(api->submit(nullptr, batch, 3, res) == 2);
    CHECK(g_seen.ops.size() == 2 && g_seen.ops[0] == FOXAPI_OP_SET_FREQUENCY && g_seen.nums[0] == 100e6 && g_seen.ops[1] == FOXAPI_OP_SET_MODE);
    CHECK(res[0].status == FOXAPI_OK && res[1].status == FOXAPI_DENIED && res[1].ticket == 0 && res[2].status == FOXAPI_OK);
    // another radio is not locked (the mask is per radio)
    g_seen = Seen{};
    FoxCommand other = cmd(FOXAPI_OP_SET_VFO_OFFSET, 2);
    FoxSubmitResult r1{};
    CHECK(api->submit(nullptr, &other, 1, &r1) == 1 && g_seen.ops.size() == 1 && g_seen.ops[0] == FOXAPI_OP_SET_VFO_OFFSET);
    // the text path
    g_seen = Seen{};
    FoxCommand t = cmd(FOXAPP_OP_VFO_TO_ABSOLUTE);
    t.num[0] = 99e6;
    FoxSubmitResult tr{};
    CHECK(api->submit_text(nullptr, &t, "x", 1, &tr) == 1 && g_seen.ops.size() == 1 && g_seen.ops[0] == FOXAPI_OP_SET_FREQUENCY);
    FoxCommand to = cmd(FOXAPI_OP_SET_VFO_OFFSET);
    CHECK(api->submit_text(nullptr, &to, "x", 1, &tr) == 0 && tr.status == FOXAPI_DENIED && g_seen.ops.size() == 1);
    // everything refused: nothing reaches the engine
    g_seen = Seen{};
    FoxCommand only = cmd(FOXAPI_OP_SET_VFO_OFFSET);
    FoxSubmitResult r2{};
    CHECK(api->submit(nullptr, &only, 1, &r2) == 0 && r2.status == FOXAPI_DENIED && g_seen.ops.empty());
    CHECK(deltaRefusals() - refusals0 >= 3 && deltaConversions() - conv0 >= 2);
    g_mask = 0;
}

void bandwidth() {
    BandwidthMemory b;
    std::map<std::uint32_t, double> mem;
    bool changed = false;
    long t = 0;
    const auto step = [&](bool on, std::uint32_t mode, double bw) { return b.update(on, mode, bw, ++t, mem, changed); };
    CHECK(!step(false, FOXAPI_DEMOD_NFM, 12500).has_value() && mem.empty());             // off: nothing
    CHECK(!step(true, FOXAPI_DEMOD_NFM, 12500).has_value());                              // seeded: the width found at start is the mode's own
    for (int i = 0; i < 20; ++i) { CHECK(!step(true, FOXAPI_DEMOD_NFM, 12500).has_value()); }
    CHECK(mem.empty());                                                                   // not recorded: the user has not moved it
    // the user narrows NFM to 8 kHz: recorded once it has settled for ten ticks
    step(true, FOXAPI_DEMOD_NFM, 8000);
    bool sawChange = false;
    for (int i = 0; i < 14; ++i) { step(true, FOXAPI_DEMOD_NFM, 8000); if (changed) { sawChange = true; } }
    CHECK(sawChange && mem.at(FOXAPI_DEMOD_NFM) == 8000.0);
    // to AM: the engine puts AM's default in (6 kHz); nothing remembered for AM, nothing sent, and the default is not recorded
    CHECK(!step(true, FOXAPI_DEMOD_AM, 12500).has_value());
    for (int i = 0; i < 30; ++i) { step(true, FOXAPI_DEMOD_AM, 6000); }
    CHECK(mem.count(FOXAPI_DEMOD_AM) == 0);
    // back to NFM: the engine's default (12.5 kHz) arrives with the mode; the remembered 8 kHz is asked for
    const auto ask = step(true, FOXAPI_DEMOD_NFM, 12500);
    CHECK(ask.has_value() && ask.value_or(-12345) == 8000.0);
    CHECK(!step(true, FOXAPI_DEMOD_NFM, 12500).has_value());                              // the op not yet applied: no second request, nothing recorded
    for (int i = 0; i < 20; ++i) { step(true, FOXAPI_DEMOD_NFM, 8000); }
    CHECK(mem.at(FOXAPI_DEMOD_NFM) == 8000.0);
    // a width the engine clamped (asked 8000, it stays 9000): after the wait it is the user's baseline, not asked again
    mem[FOXAPI_DEMOD_USB] = 2400.0;
    CHECK(step(true, FOXAPI_DEMOD_USB, 2700).value_or(-12345) == 2400.0);
    for (int i = 0; i < 80; ++i) { CHECK(!step(true, FOXAPI_DEMOD_USB, 2700).has_value()); }
    // turning it off forgets the baseline (no stale mode)
    step(false, FOXAPI_DEMOD_USB, 2700);
    CHECK(!step(true, FOXAPI_DEMOD_AM, 6000).has_value());
}

void clockRows() {
    CHECK(clockText(0, 1000, 0) == "");
    CHECK(clockText(3, 1000, 0) == "");
    CHECK(clockText(1, 0, 3600) == "00:00:00 UTC");
    CHECK(clockText(1, 1762871525, 0) == "14:32:05 UTC");                 // 2025-11-11 14:32:05 UTC
    CHECK(clockText(2, 1762871525, 3600) == "15:32:05 LT");
    CHECK(clockText(2, 1762871525, -5 * 3600) == "09:32:05 LT");
    CHECK(clockText(2, 60, -3600) == "23:01:00 LT");                      // across midnight backwards
    CHECK(clockText(1, -1, 0) == "23:59:59 UTC");                         // before the epoch
    const int off = localOffsetSecondsNow(1762871525);
    CHECK(off >= -14 * 3600 && off <= 14 * 3600 && off % 900 == 0);       // every zone is a whole quarter hour
}

void shortcuts() {
    foxsdr::slintsystem::KeyTable t = foxsdr::slintsystem::defaultKeyTable();
    const auto rows = shortcutRows(t);
    CHECK(!rows.empty());
    std::size_t bound = 0;
    for (const auto& c : t) { if (c.bound()) { ++bound; } }
    CHECK(rows.size() == bound);
    for (const auto& r : rows) { CHECK(!r.keys.empty() && !r.what.empty() && r.keys != "None"); }
    // an unbound action is not listed, a rebound one shows its new chord
    foxsdr::slintsystem::KeyTable u = t;
    u[0] = foxsdr::slintsystem::Chord{};
    CHECK(shortcutRows(u).size() == bound - (t[0].bound() ? 1u : 0u));
    foxsdr::slintsystem::Chord c;
    c.key = "F9";
    c.ctrl = true;
    u[1] = c;
    bool found = false;
    for (const auto& r : shortcutRows(u)) { if (r.keys == "Ctrl+F9") { found = true; } }
    CHECK(found);
}

void recordings() {
    namespace fs = std::filesystem;
    CHECK(listRecordings("", 10).empty());
    CHECK(listRecordings("Z:/definitely/not/here", 10).empty());
    const fs::path dir = fs::temp_directory_path() / "sc-lanec-recordings";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub");
    const auto put = [&](const char* name, std::size_t n) { std::ofstream(dir / name, std::ios::binary) << std::string(n, 'x'); };
    put("iq_20261111_120000_2400000Hz.wav", 3500);
    put("audio_20261111_120500_48000Hz.wav", 10);
    put("notes.txt", 5);                                                 // not a recording
    put("scan.csv", 7);
    put("pic.png", 9);
    put("UPPER.WAV", 4);
    std::ofstream(dir / "sub" / "deep.wav") << "x";                       // not looked into
    const auto list = listRecordings(dir.string(), 100);
    CHECK(list.size() == 5);
    bool sawIq = false, sawAudio = false, sawText = false;
    for (const auto& r : list) {
        if (r.name == "iq_20261111_120000_2400000Hz.wav") { sawIq = r.kind == "I/Q" && r.bytes == 3500 && r.unixSeconds > 1000000000; }
        if (r.name == "audio_20261111_120500_48000Hz.wav") { sawAudio = r.kind == "Audio"; }
        if (r.name == "notes.txt" || r.name == "deep.wav") { sawText = true; }
    }
    CHECK(sawIq && sawAudio && !sawText);
    CHECK(listRecordings(dir.string(), 2).size() == 2);                  // the limit
    for (std::size_t i = 1; i < list.size(); ++i) { CHECK(list[i - 1].unixSeconds >= list[i].unixSeconds); }   // newest first
    CHECK(isIqWav("a/b/iq_x.WAV") && isIqWav("x.wav") && !isIqWav("x.csv") && !isIqWav("wav"));
    fs::remove_all(dir, ec);
    CHECK(sizeText(0) == "0 B" && sizeText(1023) == "1023 B" && sizeText(1024) == "1.0 KB" && sizeText(1536) == "1.5 KB");
    CHECK(sizeText(5u * 1024 * 1024) == "5.0 MB" && sizeText(3ull * 1024 * 1024 * 1024) == "3.0 GB");
    CHECK(dateText(0) == "1970-01-01 00:00" && dateText(1762871525) == "2025-11-11 14:32");
}

void channels() {
    using cascade::core::airbandChannelName;
    CHECK(airbandChannelName(118.0e6) == "118.000 / 118.005");           // both names are said on air
    CHECK(airbandChannelName(118.0e6 + 25000.0 / 3.0) == "118.010");
    CHECK(airbandChannelName(118.0e6 + 50000.0 / 3.0) == "118.015");
    CHECK(airbandChannelName(118.025e6) == "118.025 / 118.030");
    CHECK(airbandChannelName(121.5e6) == "121.500 / 121.505");
    CHECK(airbandChannelName(136.975e6) == "136.975 / 136.980");
    CHECK(airbandChannelName(118.0e6 + 100.0) == "118.000 / 118.005");   // inside the 200 Hz tolerance
    CHECK(airbandChannelName(118.0e6 + 1000.0) == "");                   // between channels
    CHECK(airbandChannelName(100.0e6) == "");
    CHECK(airbandChannelName(137.5e6) == "");
    CHECK(airbandChannelName(std::nan("")) == "");
    // the inverse of airbandChannelHz for every 8.33 channel of the band
    int checked = 0;
    for (int khz = 118005; khz <= 136990; ++khz) {
        if (!cascade::core::airband833(khz)) { continue; }
        const std::string name = airbandChannelName(cascade::core::airbandChannelHz(khz));
        char want[16];
        std::snprintf(want, sizeof want, "%d.%03d", khz / 1000, khz % 1000);
        CHECK(name == want || name.find(std::string(" / ") + want) != std::string::npos);
        ++checked;
    }
    CHECK(checked > 2000);
}

}  // namespace

int main() {
    storeRoundTrip();
    fields();
    lockouts();
    muteFollower();
    scanWatch();
    quickAndMemory();
    delta();
    bandwidth();
    clockRows();
    shortcuts();
    recordings();
    channels();
    return testSummary("test_slint_lanec_host");
}
