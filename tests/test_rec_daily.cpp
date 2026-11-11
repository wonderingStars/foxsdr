// Daily recording subfolders (core/rec_daily.hpp): date folders in UTC and local time against std::chrono, hostile templates and subfolders, folder
// creation confined to the root (a junction and a file in the way, a missing root, racing threads) and a fuzz run over random templates.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define RECDAILY_PID _getpid()
#else
#include <unistd.h>
#define RECDAILY_PID getpid()
#endif

#include "core/rec_daily.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core::recdaily;

namespace {

fs::path scratchBase() {
    // the test's working directory is inside the build folder under ctest; never the temp or the user's folders
    fs::path p = fs::current_path() / ("recplay-daily-" + std::to_string(RECDAILY_PID));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

long long ms(int y, unsigned m, unsigned d, int hh = 0, int mm = 0, int ss = 0, int msec = 0) {
    using namespace std::chrono;
    const sys_days sd = year{y} / month{m} / day{d};
    return duration_cast<milliseconds>(sd.time_since_epoch()).count() + ((hh * 60LL + mm) * 60LL + ss) * 1000LL + msec;
}

void testDates() {
    Options o;
    o.style = Style::DashDate;
    CHECK(expandFolder(o, ms(2026, 11, 11, 12)).rel == "2026-11-11");
    CHECK(expandFolder(o, ms(2024, 2, 29, 23, 59, 59, 999)).rel == "2024-02-29");
    CHECK(expandFolder(o, ms(2024, 3, 1, 0, 0, 0, 0)).rel == "2024-03-01");
    CHECK(expandFolder(o, ms(2100, 2, 28, 23)).rel == "2100-02-28");            // 2100 is not a leap year
    CHECK(expandFolder(o, ms(2000, 2, 29, 1)).rel == "2000-02-29");
    CHECK(expandFolder(o, 0).rel == "1970-01-01");
    CHECK(expandFolder(o, -1).rel == "1969-12-31");                              // floor, not truncation
    CHECK(expandFolder(o, -86400000LL).rel == "1969-12-31");
    CHECK(expandFolder(o, -86400001LL).rel == "1969-12-30");
    o.style = Style::SlashDate; CHECK(expandFolder(o, ms(2026, 1, 5)).rel == "2026/01/05");
    o.style = Style::MonthThenDay; CHECK(expandFolder(o, ms(2026, 1, 5)).rel == "2026-01/05");
    o.style = Style::Compact; CHECK(expandFolder(o, ms(2026, 12, 31)).rel == "20261231");
    o.style = Style::None; { const Expanded e = expandFolder(o, ms(2026, 12, 31)); CHECK(e.ok && e.rel.empty()); }
    // UTC against local: 23:30 UTC on 31 December is already 1 January at +01:00 and still 31 December at -05:00
    o.style = Style::DashDate; o.clock = Clock::Utc; o.localOffsetMinutes = 60;
    CHECK(expandFolder(o, ms(2026, 12, 31, 23, 30)).rel == "2026-12-31");        // UTC ignores the offset
    o.clock = Clock::Local;
    CHECK(expandFolder(o, ms(2026, 12, 31, 23, 30)).rel == "2027-01-01");
    o.localOffsetMinutes = -300; CHECK(expandFolder(o, ms(2027, 1, 1, 4, 59)).rel == "2026-12-31");
    CHECK(expandFolder(o, ms(2027, 1, 1, 5, 0)).rel == "2027-01-01");
    o.localOffsetMinutes = 345; CHECK(expandFolder(o, ms(2026, 6, 30, 18, 15)).rel == "2026-07-01");   // +05:45
    o.localOffsetMinutes = 14 * 60; CHECK(expandFolder(o, ms(2026, 6, 30, 10, 0)).ok);
    o.localOffsetMinutes = 14 * 60 + 1; CHECK(!expandFolder(o, 0).ok && !expandFolder(o, 0).error.empty());
    o.localOffsetMinutes = -14 * 60 - 1; CHECK(!expandFolder(o, 0).ok);
    // range of years
    o.clock = Clock::Utc;
    CHECK(!expandFolder(o, ms(1899, 12, 31)).ok);
    CHECK(!expandFolder(o, ms(2201, 1, 1)).ok);
    CHECK(!expandFolder(o, 9100000000000000LL).ok);
    CHECK(expandFolder(o, ms(1900, 1, 1)).ok && expandFolder(o, ms(2200, 12, 31)).ok);
    // subfolder before the date folder
    o.style = Style::SlashDate;
    CHECK(expandFolder(o, ms(2026, 11, 11), "Radio 1").rel == "Radio 1/2026/11/11");
    CHECK(expandFolder(o, ms(2026, 11, 11), "a\\b").rel == "a/b/2026/11/11");
    CHECK(!expandFolder(o, ms(2026, 11, 11), "../x").ok);
    CHECK(!expandFolder(o, ms(2026, 11, 11), "a/b/c/d").ok);                     // 4 + 3 levels > 6
    // custom template with literal braces
    o.customTemplate = "{{x}}/{date}";
    CHECK(expandFolder(o, ms(2026, 11, 11)).rel == "{x}/2026-11-11");
}

void testDatesAgainstChrono() {
    // every day 1900-01-01 .. 2200-12-31 (109938 days) at a random time of day, in UTC and at a random local offset
    std::mt19937_64 g(99);
    Options o;
    o.style = Style::Compact;
    long days = 0;
    bool ok = true;
    using namespace std::chrono;
    const sys_days firstDay = year{1900} / January / 1, lastDay = year{2200} / December / 31;
    for (sys_days d = firstDay; d <= lastDay; d += std::chrono::days{1}) {
        const long long base = duration_cast<milliseconds>(d.time_since_epoch()).count();
        const long long t = base + static_cast<long long>(g() % 86400000ULL);
        o.clock = Clock::Utc;
        const Expanded e = expandFolder(o, t);
        const year_month_day ymd{d};
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04d%02u%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        ok = ok && e.ok && e.rel == buf;
        const int off = static_cast<int>(g() % 1681) - 840;
        o.clock = Clock::Local; o.localOffsetMinutes = off;
        const Expanded l = expandFolder(o, t);
        const sys_days dl{floor<std::chrono::days>(sys_time<milliseconds>{milliseconds{t + off * 60000LL}})};
        const year_month_day yl{dl};
        std::snprintf(buf, sizeof buf, "%04d%02u%02u", static_cast<int>(yl.year()), static_cast<unsigned>(yl.month()), static_cast<unsigned>(yl.day()));
        // the year range edge (1900-01-01 with a negative offset) is refused rather than compared
        if (l.ok) { ok = ok && l.rel == buf; } else { ok = ok && (static_cast<int>(yl.year()) < 1900 || static_cast<int>(yl.year()) > 2200); }
        ++days;
    }
    std::printf("dates against std::chrono: %ld days\n", days);
    CHECK(days == 109938);
    CHECK(ok);
}

void testHostile() {
    const char* bad[] = {
        "..", "../x", "a/../b", "a/..", "./a", "a/./b", "/abs", "\\\\srv\\share\\x", "C:/x", "C:", "a//b", "a/", "a\\", "con", "CON", "aux.txt", "NUL", "com1", "LPT9.log", "COM0",
        "a.", "a ", " a", "x<y", "x>y", "x:y", "x\"y", "x|y", "x?y", "x*y", "a/b/c/d/e/f/g", "{nope}", "{yyyy", "yyyy}", "{}", "{ date}", "{date}}", "x\ty", "x\x01y", "x\x7f" "y",
        "a/ b", "a /b", "\xC0\xAF", "a\xFF", "\xE2\x82", "\xED\xA0\x80",
    };
    int n = 0;
    for (const char* b : bad) {
        Options o; o.customTemplate = b;
        const Expanded e = expandFolder(o, ms(2026, 11, 11));
        CHECK(!e.ok);
        CHECK(!e.error.empty());
        CHECK(e.rel.empty());
        // as a subfolder too (only the cases that are not valid templates after expansion)
        Options p; p.style = Style::DashDate;
        const std::string s = b;
        if (s.find('{') == std::string::npos && s.find('}') == std::string::npos) { CHECK(!expandFolder(p, ms(2026, 11, 11), s).ok); }
        ++n;
    }
    std::printf("hostile folder paths refused: %d\n", n);
    // too long
    Options o; o.customTemplate = std::string(65, 'a');
    CHECK(!expandFolder(o, 0).ok);
    o.customTemplate = std::string(64, 'a');
    CHECK(expandFolder(o, 0).ok);
    o.customTemplate = std::string(63, 'a') + "/" + std::string(63, 'b') + "/" + std::string(63, 'c') + "/" + std::string(63, 'd');
    CHECK(!expandFolder(o, 0).ok);                                              // 255 bytes
    // accepted ones
    for (const char* good : {"Radio 1", "{date}/{yyyy}", "a/b/c/d/e/f", "caf\xC3\xA9", "\xE2\x82\xAC", "{{}}", "x.y", "a-b_c"}) {
        Options p; p.customTemplate = good;
        CHECK(expandFolder(p, ms(2026, 11, 11)).ok);
    }
    // a name that merely starts like a device is fine
    for (const char* good : {"console", "com10", "lpt", "auxiliary", "nullable", "c0m1"}) { Options p; p.customTemplate = good; CHECK(expandFolder(p, 0).ok); }
}

void testEnsure() {
    const fs::path base = scratchBase();
    const fs::path root = base / "root";
    fs::create_directories(root);
    // creation
    Ensured e = ensureFolder(root, "2026/11/11");
    CHECK(e.ok && e.created == 3 && fs::is_directory(root / "2026" / "11" / "11"));
    e = ensureFolder(root, "2026/11/11");
    CHECK(e.ok && e.created == 0);
    e = ensureFolder(root, "2026/11/12");
    CHECK(e.ok && e.created == 1);
    CHECK(ensureFolder(root, "").ok && ensureFolder(root, "").created == 0);         // no date folder: the root itself
    // missing root, file as root, empty root: nothing is created
    CHECK(!ensureFolder(base / "nope", "x").ok && !fs::exists(base / "nope"));
    { std::ofstream(base / "afile") << "x"; }
    CHECK(!ensureFolder(base / "afile", "x").ok);
    CHECK(!ensureFolder(fs::path(), "x").ok);
    // a file in the way
    { std::ofstream(root / "blocker") << "x"; }
    e = ensureFolder(root, "blocker/sub");
    CHECK(!e.ok && !e.error.empty() && !fs::exists(root / "blocker" / "sub"));
    // hostile paths never reach the disk
    const fs::path sentinel = base / "sentinel";
    fs::create_directories(sentinel);
    for (const char* b : {"../sentinel/x", "..\\sentinel\\x", "../../x", "/x", "\\x", "C:/x", "a/../../sentinel/y", "x/../../y", "con", "a:b"}) {
        e = ensureFolder(root, b);
        CHECK(!e.ok);
    }
    CHECK(fs::is_empty(sentinel));
    // junction in the way (Windows: mklink /J needs no privilege; elsewhere a symlink is tried)
    {
        const fs::path target = base / "outside";
        fs::create_directories(target);
        const fs::path link = root / "linked";
        bool made = false;
#if defined(_WIN32)
        const std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" > NUL 2>&1";
        made = std::system(cmd.c_str()) == 0 && fs::exists(link);
#else
        std::error_code ec; fs::create_directory_symlink(target, link, ec); made = !ec;
#endif
        if (made) {
            e = ensureFolder(root, "linked/inner");
            CHECK(!e.ok);
            CHECK(fs::is_empty(target));                                                       // nothing was created through the link
            std::error_code ec; fs::remove(link, ec);
        } else {
            SKIP_LINUX("could not create a junction or symlink here");
        }
    }
    // racing threads for one day
    {
        std::vector<std::thread> th;
        std::atomic<int> okc{0}, created{0};
        for (int t = 0; t < 8; ++t) {
            th.emplace_back([&] {
                for (int i = 0; i < 30; ++i) {
                    const Ensured r = ensureFolder(root, "race/2026/" + std::to_string(i) + "/day");
                    if (r.ok) { ++okc; created += r.created; }
                }
            });
        }
        for (auto& t : th) { t.join(); }
        CHECK(okc == 240);
        CHECK(created == 1 + 1 + 30 * 2);                                                       // race, 2026, then 30 x (i, day): each made once overall
    }
    // the parent of the root holds only what we put there
    std::set<std::string> names;
    for (const auto& de : fs::directory_iterator(base)) { names.insert(de.path().filename().string()); }
    CHECK(names == (std::set<std::string>{"root", "afile", "sentinel"}) || names == (std::set<std::string>{"root", "afile", "sentinel", "outside"}));
    std::error_code ec;
    fs::remove_all(base, ec);
}

void testFuzz() {
    const fs::path base = scratchBase();
    const fs::path root = base / "root";
    fs::create_directories(root);
    fs::create_directories(base / "sentinel");
    std::mt19937 g(4242);
    const std::vector<std::string> atoms = {"a", "b", "Radio", "{date}", "{yyyy}", "{mm}", "{dd}", "{compact}", "{", "}", "{{", "}}", "/", "\\", "/", ".", "..", ":", " ", "*", "?", "<", "|", "\"", "con", "aux", "nul", "com1", "\x01", "\xC3\xA9", "\xFF", "~", "$", "%", "..\\", "../", "x.y", "{bad}"};
    long accepted = 0, refused = 0;
    bool safe = true;
    for (int i = 0; i < 200000; ++i) {
        std::string t;
        const int n = 1 + static_cast<int>(g() % 8);
        for (int k = 0; k < n; ++k) { t += atoms[g() % atoms.size()]; }
        Options o; o.customTemplate = t; o.clock = (g() % 2) ? Clock::Utc : Clock::Local; o.localOffsetMinutes = static_cast<int>(g() % 1681) - 840;
        const Expanded e = expandFolder(o, ms(2020, 1, 1) + static_cast<long long>(g() % 2000000000ULL) * 1000LL);
        if (!e.ok) { ++refused; safe = safe && e.rel.empty() && !e.error.empty(); continue; }
        ++accepted;
        // independent check of the accepted result
        std::vector<std::string> comps;
        std::string cur;
        for (char c : e.rel) { if (c == '/') { comps.push_back(cur); cur.clear(); } else { cur += c; } }
        comps.push_back(cur);
        safe = safe && !e.rel.empty() && comps.size() <= 6 && e.rel.find('\\') == std::string::npos && e.rel.front() != '/' && e.rel.back() != '/';
        for (const std::string& c : comps) {
            safe = safe && !c.empty() && c != "." && c != ".." && c.size() <= 64 && c.back() != '.' && c.back() != ' ' && c.front() != ' ';
            for (char ch : c) { safe = safe && static_cast<unsigned char>(ch) >= 0x20 && std::string("<>:\"|?*\x7f").find(ch) == std::string::npos; }
            std::string up; for (char ch : c) { up += static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); }
            safe = safe && up != "CON" && up != "AUX" && up != "NUL" && up != "COM1" && up != "PRN";
        }
        if (accepted % 700 == 1) {
            const Ensured en = ensureFolder(root, e.rel);
            safe = safe && en.ok;
            if (en.ok) {
                std::error_code ec;
                const fs::path cr = fs::canonical(root, ec), cd = fs::canonical(en.dir, ec);
                safe = safe && cd.string().compare(0, cr.string().size(), cr.string()) == 0;
            }
        }
    }
    std::printf("fuzz: 200000 random templates, %ld accepted, %ld refused\n", accepted, refused);
    CHECK(safe);
    CHECK(accepted > 1000 && refused > 1000);
    std::set<std::string> names;
    for (const auto& de : fs::directory_iterator(base)) { names.insert(de.path().filename().string()); }
    CHECK(names == (std::set<std::string>{"root", "sentinel"}));
    CHECK(fs::is_empty(base / "sentinel"));
    std::error_code ec;
    fs::remove_all(base, ec);
}

}  // namespace

int main() {
    testDates();
    testDatesAgainstChrono();
    testHostile();
    testEnsure();
    testFuzz();
    return testSummary("test_rec_daily");
}
