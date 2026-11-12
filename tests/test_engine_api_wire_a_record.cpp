// test_engine_api_wire_a_record.cpp - THE RECORDING GROUP OF WIRING LANE wire-a, held against the real engine through its API: a real take on the generator source, written to a scratch recordings folder.
//
//   R1  DAILY FOLDERS: with the default (none) a take lands straight in the recordings folder; with style 1 it lands in <recordings>/<today, UTC>/, with style 4 in <recordings>/<yyyymmdd>/; with a file-name
//       template as well the template's folders come below the day's; a bad style / template / JSON is refused and changes nothing; the same setting again is NO_CHANGE; the setting reaches
//       record-daily.json and a new engine reads it; a junction in the way keeps the take in its default place.
//
//   L1  LOCK WHILE RECORDING: during an I/Q take the sample rate, the source and the centre are refused with the model's sentence (and nothing moves), re-sending the frozen centre is allowed, the VFO may
//       still move; after the take every one is allowed again; an audio take freezes none of them; the gain lock is its own switch; the lock can be turned off and the setting is kept in record-lock.json.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

std::vector<fs::path> wavsUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return out; }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".wav") { out.push_back(e.path()); }
    }
    return out;
}

std::string utcDate(bool compact) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, compact ? "%Y%m%d" : "%Y-%m-%d", &tm);
    return buf;
}

// One audio take: start, wait for the file, stop.
std::vector<fs::path> take(Rig& r, const fs::path& rec, std::size_t already) {
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return wavsUnder(rec).size() > already; }));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    r.settle(700);
    return wavsUnder(rec);
}

void testDaily() {
    std::printf("R1: daily folders\n");
    const fs::path rec = g_dir / "rec1", user = g_dir / "user1";
    fs::create_directories(rec);
    {
        Rig r(user, rec);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.1e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        // default: straight in the folder
        std::vector<fs::path> w = take(r, rec, 0);
        CHECK_EQ(w.size(), std::size_t(1));
        if (!w.empty()) { CHECK(w[0].parent_path() == rec); }
        // refusals
        const std::string bad1 = R"({"style":9})", bad2 = "{nope", bad3 = R"({"template":"{nosuch}"})", bad4 = R"({"template":"../x"})", bad5 = R"({"utc":"yes"})";
        for (const std::string* b : {&bad1, &bad2, &bad3, &bad4, &bad5}) { CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), b).status(), FOXAPI_BAD_ARGUMENT); }
        const std::string none = R"({"style":0})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &none).status(), FOXAPI_NO_CHANGE);   // that is the default
        // style 1: <rec>/<today>/
        const std::string s1 = R"({"style":1,"utc":true})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s1).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s1).status(), FOXAPI_NO_CHANGE);
        CHECK(r.fact("record.daily").find("\"style\":1") != std::string::npos);
        w = take(r, rec, 1);
        CHECK_EQ(w.size(), std::size_t(2));
        bool inDay = false;
        for (const fs::path& p : w) { inDay = inDay || (p.parent_path() == rec / utcDate(false)); }
        CHECK(inDay);
        // style 4 with a file-name template: <rec>/<yyyymmdd>/<template folder>/<name>
        const std::string s4 = R"({"style":4})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s4).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_NAME, 0.0, 0, "{freq_mhz}/{mode}_{year}")).status(), FOXAPI_OK);
        w = take(r, rec, 2);
        CHECK_EQ(w.size(), std::size_t(3));
        bool deep = false;
        for (const fs::path& p : w) { deep = deep || (p.parent_path() == rec / utcDate(true) / "100.1"); }
        CHECK(deep);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_NAME, 0.0, 0, "")).status(), FOXAPI_OK);
        // the setting reaches the file
        CHECK(r.until([&] { return fs::exists(user / "record-daily.json"); }, 6000));
    }
    {
        Rig r(user, rec);
        r.settle(200);
        CHECK(r.fact("record.daily").find("\"style\":4") != std::string::npos);   // a new engine reads it
    }
}

void testJunction() {
    std::printf("R2: a link in the way\n");
    const fs::path rec = g_dir / "rec2", user = g_dir / "user2", elsewhere = g_dir / "elsewhere";
    fs::create_directories(rec);
    fs::create_directories(elsewhere);
#if defined(_WIN32)
    // A junction made without a shell or a privilege: mklink /J through cmd would raise nothing visible, but the model's own test makes its own: here a directory symlink is not allowed without a
    // privilege, so the junction is made with the Win32 call the model's test uses (CreateSymbolicLink needs Developer Mode); when neither can be made the check is reported as skipped.
    const std::string day = utcDate(false);
    std::error_code ec;
    fs::create_directory_symlink(elsewhere, rec / day, ec);
    if (ec) {
        std::printf("  SKIPPED: no link could be made here (%s)\n", ec.message().c_str());
        return;
    }
#else
    std::error_code ec;
    fs::create_directory_symlink(elsewhere, rec / utcDate(false), ec);
#endif
    Rig r(user, rec);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.1e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    const std::string s1 = R"({"style":1,"utc":true})";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s1).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !wavsUnder(rec).empty(); }));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    r.settle(700);
    CHECK(wavsUnder(elsewhere).empty());   // nothing went through the link
    const std::vector<fs::path> w = wavsUnder(rec);
    // (the symlink itself is a directory entry: recursive_directory_iterator does not follow it, so the take is the one directly in the folder)
    CHECK(!w.empty());
    if (!w.empty()) { CHECK(w[0].parent_path() == rec); }
}

void testLock() {
    std::printf("L1: lock while recording\n");
    const fs::path rec = g_dir / "rec3", user = g_dir / "user3";
    fs::create_directories(rec);
    {
        Rig r(user, rec);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock"), std::string("1"));
        // an I/Q take
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return !wavsUnder(rec).empty(); }));
        testing::Outcome o = r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0));
        CHECK_EQ(o.status(), FOXAPI_DENIED);
        CHECK(std::string(o.result.message).find("centre frequency is locked while an I/Q recording is open") != std::string::npos);
        CHECK(r.fact("record.lock.reason").find("Stop the recording first") != std::string::npos);   // the whole sentence (the reply's message is short)
        CHECK(r.fact("record.lock.reason").find("the recording stores one centre frequency") != std::string::npos);
        CHECK_EQ(r.fact("tuned.1.centreHz"), std::string("100000000"));   // nothing moved
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);   // the frozen value again is allowed
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SAMPLE_RATE, 1.0e6, 0)).status(), FOXAPI_DENIED);
        FoxCommand src = testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, "siggen");
        CHECK_EQ(r.send(src).status(), FOXAPI_DENIED);
        CHECK(r.fact("record.lock.frozen.1").find("sample rate") != std::string::npos);
        CHECK(r.fact("record.lock.frozen.1").find("centre frequency") != std::string::npos);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 5000.0, 0)).status(), FOXAPI_OK);   // the VFO is the user's to move
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_GAIN, 10.0, 0)).status() == FOXAPI_DENIED, false);   // gain is free unless the gain lock is on
        // the take ends: everything is allowed again
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock.frozen.1"), std::string());
        // an audio take freezes none of these
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
        r.settle(300);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 102.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        // the switch: off allows the change during a take; on again refuses; the same setting is NO_CHANGE; a bad value is refused
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 5)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock"), std::string("0"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        r.settle(300);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 103.0e6, 0)).status(), FOXAPI_OK);   // not locked
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        // gain lock: its own switch
        FoxCommand both = testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1);
        both.ival[1] = 1;
        CHECK_EQ(r.send(both).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock.gain"), std::string("1"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        r.settle(300);
        // (the generator has no device, so SET_GAIN is answered NO_DEVICE before the engine's apply: the gain lock is held by its switch and facts here, UNVERIFIED against a real device)
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK(r.until([&] {
            std::ifstream f(user / "record-lock.json");
            std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return t.find("\"gain\":true") != std::string::npos;
        }, 6000));
    }
    {
        Rig r(user, rec);
        r.settle(200);
        CHECK_EQ(r.fact("record.lock.gain"), std::string("1"));   // a new engine reads the switches
    }
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_record");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_record_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testDaily();
    testJunction();
    testLock();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_record");
}
