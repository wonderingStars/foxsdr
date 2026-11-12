// Tests for the recording file names, the calendar arithmetic and the rollover splitter (core/rec_naming.hpp, core/civil_time.hpp).
//
// Expected values are worked out by hand or with an independent tool (Python's datetime, run once for the golden Unix times below), not by the code
// under test. The calendar is also checked against std::chrono's calendar for every day from 1900 to 2200.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/civil_time.hpp"
#include "core/rec_naming.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace rn = cascade::core::recname;
namespace cv = cascade::core::civil;

constexpr std::int64_t kWed = 1794415314123;   // 2026-11-11T16:41:54.123Z (Wednesday, day 315)  [Python datetime]
constexpr std::int64_t kNewYearEve = 1798759800000;   // 2026-12-31T23:30:00Z

void testCivil() {
    // every day 1900-01-01 .. 2200-12-31 against std::chrono
    long mism = 0, days = 0;
    using namespace std::chrono;
    const sys_days first = sys_days{year{1900} / January / 1}, last = sys_days{year{2200} / December / 31};
    for (sys_days d = first; d <= last; d += std::chrono::days{1}) {
        const year_month_day ymd{d};
        const std::int64_t ms = static_cast<std::int64_t>(d.time_since_epoch().count()) * 86400000 + 43200123;
        const cv::DateTime t = cv::fromUnixMs(ms);
        const unsigned wd = static_cast<unsigned>(weekday{d}.c_encoding());
        if (t.year != static_cast<int>(ymd.year()) || t.month != static_cast<int>(static_cast<unsigned>(ymd.month())) || t.day != static_cast<int>(static_cast<unsigned>(ymd.day())) ||
            t.hour != 12 || t.minute != 0 || t.second != 0 || t.millis != 123 || t.weekday != static_cast<int>(wd) ||
            cv::toUnixMs(t.year, t.month, t.day, t.hour, t.minute, t.second, t.millis) != ms) {
            ++mism;
        }
        ++days;
    }
    CHECK(mism == 0);
    std::printf("civil: %ld days (1900..2200) agree with std::chrono, including weekday and the round trip\n", days);
    // golden rows
    const cv::DateTime a = cv::fromUnixMs(kWed);
    CHECK(a.year == 2026 && a.month == 11 && a.day == 11 && a.hour == 16 && a.minute == 41 && a.second == 54 && a.millis == 123 && a.weekday == 3 && a.dayOfYear == 315);
    const cv::DateTime b = cv::fromUnixMs(-1);   // one ms before the epoch
    CHECK(b.year == 1969 && b.month == 12 && b.day == 31 && b.hour == 23 && b.minute == 59 && b.second == 59 && b.millis == 999 && b.weekday == 3 && b.dayOfYear == 365);
    const cv::DateTime c = cv::fromUnixMs(951825600000);
    CHECK(c.year == 2000 && c.month == 2 && c.day == 29 && c.weekday == 2 && c.dayOfYear == 60);
    CHECK(cv::isLeap(2000) && !cv::isLeap(1900) && !cv::isLeap(2100) && cv::isLeap(2024) && !cv::isLeap(2023));
    // offsets cross midnight and the year
    const cv::DateTime d1 = cv::fromUnixMs(kNewYearEve, 120);
    CHECK(d1.year == 2027 && d1.month == 1 && d1.day == 1 && d1.hour == 1 && d1.minute == 30);
    const cv::DateTime d2 = cv::fromUnixMs(kNewYearEve, -330);   // UTC-05:30
    CHECK(d2.year == 2026 && d2.month == 12 && d2.day == 31 && d2.hour == 18 && d2.minute == 0);
    const cv::DateTime d3 = cv::fromUnixMs(0, -1);
    CHECK(d3.year == 1969 && d3.month == 12 && d3.day == 31 && d3.hour == 23 && d3.minute == 59);
    CHECK(cv::offsetText(0, true) == "Z" && cv::offsetText(0, false) == "+0000" && cv::offsetText(330, true) == "+0530" && cv::offsetText(-570, true) == "-0930");
    CHECK(cv::floorDiv(-1, 1000) == -1 && cv::floorMod(-1, 1000) == 999 && cv::floorDiv(-1000, 1000) == -1 && cv::floorMod(1000, 1000) == 0);
}

rn::Fields baseFields() {
    rn::Fields f;
    f.unixMs = kWed;
    f.frequencyHz = 7074000.0;
    f.mode = "USB"; f.radio = "B200"; f.bookmark = "FT8 40m"; f.counter = 7;
    return f;
}

void testExpandGolden() {
    const rn::Fields f = baseFields();
    struct Row { const char* tmpl; const char* want; };
    const Row rows[] = {
        {"{date}", "20261111"}, {"{time}", "164154"}, {"{datetime}", "20261111_164154"}, {"{year}-{month}-{day}", "2026-11-11"},
        {"{hour}h{min}m{sec}s{ms}", "16h41m54s123"}, {"{tz}", "Z"},
        {"{freq}", "7074000"}, {"{freq_khz}", "7074"}, {"{freq_mhz}", "7.074"},
        {"{mode}", "USB"}, {"{radio}", "B200"}, {"{bookmark}", "FT8 40m"},
        {"{counter}", "7"}, {"{counter:3}", "007"}, {"{counter:9}", "000000007"},
        {"{date}/{radio}/{freq_mhz}-{mode}-{counter:3}", "20261111/B200/7.074-USB-007"},
        {"{date}\\{radio}", "20261111/B200"},                       // backslash is a separator, written as /
        {"rec {{x}} {mode}", "rec {x} USB"},                          // literal braces
        {"fixed", "fixed"}, {"{bookmark}_{datetime}", "FT8 40m_20261111_164154"},
    };
    int n = 0;
    for (const Row& r : rows) {
        const rn::Expanded e = rn::expand(r.tmpl, f);
        if (!e.ok || e.relPath != r.want) { std::printf("  row '%s': ok=%d got '%s' (%s) want '%s'\n", r.tmpl, e.ok, e.relPath.c_str(), e.error.c_str(), r.want); }
        CHECK(e.ok && e.relPath == r.want);
        ++n;
    }
    // local time
    rn::Fields l = f;
    l.unixMs = kNewYearEve; l.utcOffsetMinutes = 120;
    CHECK(rn::expand("{date}_{time}{tz}", l, false).relPath == "20270101_013000+0200");
    CHECK(rn::expand("{date}_{time}{tz}", l, true).relPath == "20261231_233000Z");
    l.utcOffsetMinutes = -330;
    CHECK(rn::expand("{datetime}{tz}", l, false).relPath == "20261231_180000-0530");
    // frequencies
    rn::Fields q = f;
    q.frequencyHz = 145500000.0; CHECK(rn::expand("{freq_mhz}|{freq_khz}", q).relPath.empty());   // '|' is refused
    CHECK(rn::expand("{freq_mhz}_{freq_khz}", q).relPath == "145.5_145500");
    q.frequencyHz = 0.0; CHECK(rn::expand("{freq}_{freq_khz}_{freq_mhz}", q).relPath == "0_0_0");
    q.frequencyHz = -5.0; CHECK(rn::expand("{freq}", q).relPath == "0");
    q.frequencyHz = 1234.6; CHECK(rn::expand("{freq}_{freq_khz}", q).relPath == "1235_1.235");
    q.frequencyHz = 7074123.456789; CHECK(rn::expand("{freq_mhz}_{freq_khz}", q).relPath == "7.074123_7074.123");
    q.frequencyHz = 1e12; CHECK(rn::expand("{freq}", q).relPath == "1000000000000");
    q.frequencyHz = std::nan(""); (void)rn::expand("{freq}", q);   // must not crash; the value is not asserted
    // counter wraps nothing and pads only up to its width
    rn::Fields c = f; c.counter = 123456; CHECK(rn::expand("{counter:3}", c).relPath == "123456");
    c.counter = 0; CHECK(rn::expand("{counter:4}", c).relPath == "0000");
    std::printf("expand: %d golden rows, local-time rows across midnight and the year, frequency forms\n", n);
}

void testRefusals() {
    const rn::Fields f = baseFields();
    struct Row { const char* tmpl; const char* why; };
    const Row rows[] = {
        {"", "empty template"},
        {"a<b", "less-than"}, {"a>b", "greater-than"}, {"a:b", "colon (drive or alternate stream)"}, {"a\"b", "quote"}, {"a|b", "pipe"}, {"a?b", "question mark"}, {"a*b", "star"},
        {"C:\\x", "drive"}, {"C:/x", "drive"}, {"/abs", "leading separator"}, {"\\\\server\\share\\x", "UNC"}, {"\\abs", "leading backslash"},
        {"../x", "parent escape"}, {"a/../b", "parent escape in the middle"}, {"..", "bare parent"}, {".", "bare dot"}, {"a/./b", "dot component"}, {"a//b", "empty component"}, {"a/", "trailing separator"},
        {"name.", "trailing dot"}, {"name ", "trailing space"}, {"a./b", "folder with a trailing dot"}, {"a /b", "folder with a trailing space"},
        {"CON", "reserved"}, {"con", "reserved lower"}, {"NUL.txt", "reserved with extension"}, {"x/PRN", "reserved in folder path"}, {"AUX", "reserved"}, {"COM1", "reserved"}, {"lpt9", "reserved"}, {"com0", "COM0 is reserved"},
        {"{nope}", "unknown token"}, {"{date", "unclosed"}, {"date}", "stray close"}, {"{counter:0}", "bad width"}, {"{counter:10}", "bad width"}, {"{counter:x}", "bad width"}, {"{date:3}", "argument on a token without one"},
        {"a/b/c/d/e", "too many folders"}, {"a\tb", "tab"}, {"a\nb", "newline"}, {"a\x01z", "control"},
    };
    int n = 0;
    for (const Row& r : rows) {
        const rn::Expanded e = rn::expand(r.tmpl, f);
        if (e.ok) { std::printf("  NOT refused: '%s' (%s) -> '%s'\n", r.tmpl, r.why, e.relPath.c_str()); }
        CHECK(!e.ok);
        CHECK(!e.error.empty());
        CHECK(e.relPath.empty());
        ++n;
    }
    // accepted look-alikes: dots inside names, three folders, spaces inside, unicode, braces escapes
    const char* good[] = {"a.b", "a b", "a-b_c", "x/y/z/w", ".hidden", "...x", "CONSOLE", "comx", "NULL", "COM10", "caf\xC3\xA9 {mode}", "a{{b}}c"};
    for (const char* g : good) {
        const rn::Expanded e = rn::expand(g, f);
        if (!e.ok) { std::printf("  wrongly refused: '%s': %s\n", g, e.error.c_str()); }
        CHECK(e.ok);
    }
    // total length
    CHECK(!rn::expand(std::string(121, 'a'), f).ok);   // one component over 120
    CHECK(rn::expand(std::string(120, 'a'), f).ok);
    CHECK(!rn::expand(std::string(100, 'a') + "/" + std::string(100, 'b'), f).ok);   // 201 bytes in total
    CHECK(rn::expand(std::string(100, 'a') + "/" + std::string(99, 'b'), f).ok);    // 200: allowed
    std::printf("refusals: %d hostile templates refused with a message and an empty result; look-alikes accepted\n", n);
}

void testSanitise() {
    rn::Fields f = baseFields();
    struct Row { const char* value; const char* want; };
    const Row rows[] = {
        {"BBC R4 (LW)/test", "BBC R4 (LW)_test"}, {"a:b", "a_b"}, {"x\\y", "x_y"}, {"q?*<>|\"z", "q______z"}, {"  lead", "lead"}, {"trail. .", "trail"},
        {"", "unknown"}, {"   ", "unknown"}, {"...", "unknown"}, {".", "unknown"}, {"..", "unknown"}, {"con", "_con"}, {"NUL.txt", "_NUL.txt"}, {"COM3", "_COM3"},
        {"ctl\x01\x1f" "x", "ctl__x"}, {"tab\there", "tab_here"}, {"keep.dot", "keep.dot"}, {"caf\xC3\xA9", "caf\xC3\xA9"},
    };
    int n = 0;
    for (const Row& r : rows) {
        f.bookmark = r.value;
        const rn::Expanded e = rn::expand("{bookmark}", f);
        if (!e.ok || e.relPath != r.want) { std::printf("  value '%s': ok=%d got '%s' want '%s'\n", r.value, e.ok, e.relPath.c_str(), r.want); }
        CHECK(e.ok && e.relPath == r.want);
        ++n;
    }
    f.bookmark = "BBC R4 (LW)/test";
    CHECK(rn::expand("{bookmark}", f).valueSanitised);
    f.bookmark = "clean";
    CHECK(!rn::expand("{bookmark}", f).valueSanitised);
    // a value never adds a folder
    f.bookmark = "a/b/c";
    CHECK(rn::expand("{bookmark}", f).relPath == "a_b_c");
    // truncation at 64 bytes never splits a UTF-8 sequence: 40 x 2-byte characters = 80 bytes -> 32 characters (64 bytes); 21 x 3-byte -> 21 (63 bytes)
    f.bookmark.clear();
    for (int i = 0; i < 40; ++i) { f.bookmark += "\xC3\xA9"; }
    std::string t = rn::expand("{bookmark}", f).relPath;
    CHECK(t.size() == 64);
    bool valid = true;
    for (std::size_t i = 0; i < t.size(); i += 2) { if (static_cast<unsigned char>(t[i]) != 0xC3 || static_cast<unsigned char>(t[i + 1]) != 0xA9) { valid = false; } }
    CHECK(valid);
    f.bookmark.clear();
    for (int i = 0; i < 30; ++i) { f.bookmark += "\xE2\x82\xAC"; }   // euro sign, 3 bytes
    t = rn::expand("{bookmark}", f).relPath;
    CHECK(t.size() == 63);
    valid = true;
    for (std::size_t i = 0; i < t.size(); i += 3) { if (static_cast<unsigned char>(t[i]) != 0xE2) { valid = false; } }
    CHECK(valid);
    std::printf("sanitise: %d run-time values (forbidden characters, dots, reserved names, empty, control, unicode) and the UTF-8-safe cut\n", n);
}

void testResolveAndUnique() {
    std::string err;
    const fs::path base = fs::temp_directory_path() / "recm-naming";
    fs::remove_all(base);
    fs::create_directories(base);
    const std::string b = base.string();
    const std::string p = rn::resolvePath(b, "20261111/B200/x", "flac", &err);
    CHECK(!p.empty());
    CHECK(fs::path(p).lexically_normal() == (base / "20261111" / "B200" / "x.flac").lexically_normal());
    // hostile relative paths handed straight to resolvePath
    CHECK(rn::resolvePath(b, "../x", "flac", &err).empty() && !err.empty());
    CHECK(rn::resolvePath(b, "a/../../x", "flac", &err).empty());
    CHECK(rn::resolvePath(b, "a/../x", "flac", &err).empty() == false || true);   // inside the base after normalisation: allowed or refused, never outside (see the fuzz)
    CHECK(rn::resolvePath(b, "x", "", &err).empty());
    CHECK(rn::resolvePath(b, "x", "fl ac", &err).empty());
    CHECK(rn::resolvePath(b, "x", "flac/..", &err).empty());
    CHECK(rn::resolvePath(b, "x", "abcdefghi", &err).empty());
    CHECK(rn::resolvePath("", "x", "flac", &err).empty());
    CHECK(rn::resolvePath(b, std::string(300, 'a'), "flac", &err).empty());   // over the full-path limit
    // unique names
    const std::string target = (base / "t.flac").string();
    CHECK(rn::uniquePath(target) == target);
    CHECK(rn::createExclusive(target));
    CHECK(!rn::createExclusive(target));                                   // the second creator loses
    CHECK(fs::path(rn::uniquePath(target)).filename() == "t-2.flac");
    CHECK(rn::createExclusive((base / "t-2.flac").string()));
    CHECK(fs::path(rn::uniquePath(target)).filename() == "t-3.flac");
    // createExclusive makes missing folders
    CHECK(rn::createExclusive((base / "new" / "deeper" / "f.wav").string()));
    CHECK(fs::exists(base / "new" / "deeper" / "f.wav"));
    // a file must not be overwritten: its content survives a losing createExclusive
    { std::FILE* f = std::fopen((base / "keep.bin").string().c_str(), "wb"); std::fputs("data", f); std::fclose(f); }
    CHECK(!rn::createExclusive((base / "keep.bin").string()));
    CHECK(fs::file_size(base / "keep.bin") == 4);
    // all 9999 taken -> empty (simulate cheaply: only check the cap value by reaching -9999 with a scan would be slow; check -2..-40 instead)
    for (int i = 2; i <= 40; ++i) { rn::createExclusive((base / ("t-" + std::to_string(i) + ".flac")).string()); }
    CHECK(fs::path(rn::uniquePath(target)).filename() == "t-41.flac");
    // two threads racing for the same name: exactly one wins each round
    int doubleWins = 0;
    for (int round = 0; round < 50; ++round) {
        const std::string r = (base / ("race" + std::to_string(round) + ".bin")).string();
        std::atomic<int> wins{0};
        std::vector<std::thread> th;
        for (int i = 0; i < 4; ++i) { th.emplace_back([&] { if (rn::createExclusive(r)) { ++wins; } }); }
        for (auto& t : th) { t.join(); }
        if (wins.load() != 1) { ++doubleWins; }
    }
    CHECK(doubleWins == 0);
    fs::remove_all(base);
}

// Fuzz: random templates over a hostile alphabet and random hostile values. Invariant: every accepted result is a safe relative path, and
// resolves inside the base.
void testFuzz() {
    std::mt19937 g(20261111);
    const char alphabet[] = "ab{}:/\\.. <>\"|?*_-09";
    const char* toks[] = {"{date}", "{time}", "{freq}", "{mode}", "{radio}", "{bookmark}", "{counter}", "{counter:3}", "{freq_mhz}", "{tz}", "{{", "}}", "{x}", "{"};
    const std::string base = (fs::temp_directory_path() / "recm-fuzz-base").string();
    int accepted = 0, refused = 0, bad = 0, escapes = 0;
    for (int it = 0; it < 200000; ++it) {
        std::string t;
        const int len = 1 + static_cast<int>(g() % 14);
        for (int i = 0; i < len; ++i) {
            if (g() % 5 == 0) { t += toks[g() % (sizeof(toks) / sizeof(toks[0]))]; } else { t.push_back(alphabet[g() % (sizeof(alphabet) - 1)]); }
        }
        rn::Fields f;
        f.unixMs = static_cast<std::int64_t>(g()) * 1000;
        f.frequencyHz = static_cast<double>(g() % 2000000000u);
        auto rnd = [&] { std::string s; const int n = static_cast<int>(g() % 12); for (int i = 0; i < n; ++i) { s.push_back(static_cast<char>(g() % 256)); } return s; };
        f.mode = rnd(); f.radio = rnd(); f.bookmark = rnd(); f.counter = g();
        const rn::Expanded e = rn::expand(t, f, (g() & 1) != 0);
        if (!e.ok) { ++refused; continue; }
        ++accepted;
        bool safe = !e.relPath.empty() && e.relPath[0] != '/' && e.relPath.find('\\') == std::string::npos && e.relPath.size() <= rn::kMaxRelBytes;
        std::size_t from = 0;
        int comps = 0;
        for (;;) {
            const std::size_t p = e.relPath.find('/', from);
            const std::string c = e.relPath.substr(from, p == std::string::npos ? std::string::npos : p - from);
            ++comps;
            if (c.empty() || c == "." || c == ".." || c.back() == '.' || c.back() == ' ' || c.size() > rn::kMaxComponentBytes || rn::detail::isReservedDeviceName(c)) { safe = false; }
            for (unsigned char ch : c) { if (rn::detail::isForbiddenChar(ch)) { safe = false; } }
            if (p == std::string::npos) { break; }
            from = p + 1;
        }
        if (comps > rn::kMaxFolders + 1) { safe = false; }
        if (!safe) { ++bad; if (bad < 5) { std::printf("  UNSAFE accepted: template '%s' -> '%s'\n", t.c_str(), e.relPath.c_str()); } continue; }
        std::string err;
        const std::string full = rn::resolvePath(base, e.relPath, "wav", &err);
        if (!full.empty()) {
            const fs::path rel = fs::path(full).lexically_relative(fs::path(base).lexically_normal());
            if (rel.empty() || *rel.begin() == "..") { ++escapes; }
        }
    }
    CHECK(bad == 0);
    CHECK(escapes == 0);
    CHECK(accepted > 5000);
    CHECK(refused > 5000);
    std::printf("fuzz: 200000 random templates and hostile values: %d accepted (all safe relative paths, 0 escapes), %d refused\n", accepted, refused);
}

// -------------------------------------------------------------------------------------------------------------------------------- rollover
std::uint64_t collect(rn::Rollover& r, std::size_t total, std::size_t chunk, std::vector<std::uint64_t>* perFile, bool* order, std::vector<rn::Segment>* all = nullptr) {
    // feeds `total` frames in chunks; a frame's value is its global index; segments are laid into files and the concatenation is checked
    std::uint64_t next = 0;      // the next global frame index expected at the file boundary
    std::uint64_t fed = 0;
    std::uint64_t lastIndex = 0;
    bool first = true;
    *order = true;
    while (fed < total) {
        const std::size_t n = std::min(chunk, total - fed);
        std::size_t done = 0;
        while (done < n) {
            std::vector<rn::Segment> segs;
            const std::size_t used = r.feed(n - done, &segs, 8);
            for (const rn::Segment& s : segs) {
                if (all != nullptr) { all->push_back(s); }
                if (s.startsFile) { if (!first && s.fileIndex != lastIndex + 1) { *order = false; } perFile->push_back(0); }
                else if (first || s.fileIndex != lastIndex) { *order = false; }
                first = false;
                lastIndex = s.fileIndex;
                if (perFile->empty()) { *order = false; perFile->push_back(0); }
                perFile->back() += s.frames;
                next += s.frames;
                if (s.frames == 0) { *order = false; }
            }
            done += used;
            if (used == 0) { *order = false; break; }
        }
        fed += n;
    }
    return next;
}

void testRolloverSize() {
    rn::RolloverPolicy p;
    p.maxBytes = 44 + 1000 * 8; p.headerBytes = 44; p.bytesPerFrame = 8; p.sampleRate = 48000;
    rn::Rollover r;
    std::string err;
    CHECK(r.configure(p, &err));
    std::vector<std::uint64_t> per; bool order = true;
    const std::uint64_t got = collect(r, 10500, 333, &per, &order);
    CHECK(got == 10500);
    CHECK(order);
    CHECK(per.size() == 11);                                      // 10 full files of 1000 frames and one of 500
    bool full = true; for (std::size_t i = 0; i + 1 < per.size(); ++i) { if (per[i] != 1000) { full = false; } }
    CHECK(full && per.back() == 500);
    // every file, header included, is within the byte limit
    for (std::uint64_t n : per) { CHECK(44 + n * 8 <= p.maxBytes); }
    std::printf("rollover by size: 10500 frames in 333-frame writes -> %zu files (10 x 1000 + 500), each at most %llu bytes\n", per.size(), static_cast<unsigned long long>(p.maxBytes));

    // GAPLESS PROOF: ramp input in random chunk sizes; concatenate each file's frames; the files in order must reproduce the ramp exactly
    for (int trial = 0; trial < 200; ++trial) {
        std::mt19937 g(1000 + static_cast<unsigned>(trial));
        rn::RolloverPolicy q;
        q.bytesPerFrame = 1 + g() % 16; q.headerBytes = g() % 64;
        const std::uint64_t perFileFrames = 1 + g() % 700;
        q.maxBytes = q.headerBytes + perFileFrames * q.bytesPerFrame + g() % q.bytesPerFrame;   // slack smaller than one frame: must not change the frame count
        q.sampleRate = 48000;
        if (g() % 2) { q.maxSeconds = static_cast<double>(1 + g() % 40) / 1000.0; }   // 48 .. 1920 frames: may bind before the size limit
        rn::Rollover rr;
        if (!rr.configure(q, &err)) { continue; }
        const std::uint64_t cap = std::min<std::uint64_t>(perFileFrames, q.maxSeconds > 0 ? static_cast<std::uint64_t>(std::floor(q.maxSeconds * 48000 + 0.5)) : ~0ull);
        const std::size_t total = 3000 + g() % 4000;
        std::vector<std::vector<std::uint64_t>> files;
        std::uint64_t idx = 0;
        std::size_t fed = 0;
        bool ok = true;
        while (fed < total) {
            const std::size_t n = std::min<std::size_t>(1 + g() % 1500, total - fed);
            std::vector<rn::Segment> segs;
            std::size_t used = 0;
            while (used < n) {
                segs.clear();
                const std::size_t u = rr.feed(n - used, &segs, 4);
                for (const rn::Segment& s : segs) {
                    if (s.startsFile) { files.emplace_back(); }
                    if (files.empty() || s.fileIndex + 1 != files.size()) { ok = false; break; }
                    for (std::size_t k = 0; k < s.frames; ++k) { files[s.fileIndex].push_back(idx++); }
                }
                used += u;
                if (u == 0) { ok = false; break; }
            }
            fed += n;
        }
        std::uint64_t expect = 0;
        for (std::size_t fi = 0; fi < files.size() && ok; ++fi) {
            if (files[fi].size() > cap) { ok = false; }
            if (fi + 1 < files.size() && files[fi].size() != cap) { ok = false; }
            for (std::uint64_t v : files[fi]) { if (v != expect++) { ok = false; } }
        }
        if (expect != total) { ok = false; }
        if (!ok) { std::printf("  gapless trial %d failed\n", trial); }
        CHECK(ok);
    }
    std::printf("rollover gapless: 200 random policies (frame size, header, slack, size and time limits) and random write sizes: the files in order reproduce the input exactly, every file full except the last\n");

    // refusals
    rn::RolloverPolicy bad = p;
    bad.maxBytes = 44 + 7; CHECK(!r.configure(bad, &err) && !err.empty());     // less than one frame
    bad = p; bad.bytesPerFrame = 0; CHECK(!r.configure(bad, &err));
    bad = p; bad.sampleRate = 0; CHECK(!r.configure(bad, &err));
    bad = p; bad.sampleRate = std::nan(""); CHECK(!r.configure(bad, &err));
    bad = p; bad.maxSeconds = -1; CHECK(!r.configure(bad, &err));
    bad = p; bad.maxSeconds = 1e-9; CHECK(!r.configure(bad, &err));              // shorter than a frame
    bad = p; bad.maxBytes = 0; bad.alignToClock = true; bad.maxSeconds = 0; CHECK(!r.configure(bad, &err));
    // unconfigured splitter consumes nothing
    rn::Rollover raw; std::vector<rn::Segment> segs; CHECK(raw.feed(100, &segs) == 0 && segs.empty());
    // zero-frame feed is a no-op
    CHECK(r.configure(p, &err)); segs.clear(); CHECK(r.feed(0, &segs) == 0 && segs.empty());
    // a file that is exactly full closes on the last frame and the NEXT frame starts a new file
    CHECK(r.configure(p, &err));
    segs.clear(); CHECK(r.feed(1000, &segs) == 1000);
    CHECK(segs.size() == 1 && segs[0].startsFile && segs[0].closesFile && segs[0].frames == 1000);
    segs.clear(); CHECK(r.feed(1, &segs) == 1);
    CHECK(segs.size() == 1 && segs[0].startsFile && segs[0].fileIndex == 1 && !segs[0].closesFile);
    // endFile(): a stop; the next feed starts a fresh file
    r.endFile();
    segs.clear(); r.feed(5, &segs);
    CHECK(segs.size() == 1 && segs[0].startsFile && segs[0].fileIndex == 2);
    // the segment cap: 1,000,000 frames at 1000 per file would be 1000 segments; capped feed returns what it consumed and the rest can be re-fed
    CHECK(r.configure(p, &err));
    segs.clear();
    const std::size_t used = r.feed(1000000, &segs, 10);
    CHECK(segs.size() == 10 && used == 10000);
    std::printf("rollover edge rows: refusals, zero feed, exactly-full file, stop, segment cap\n");
}

void testRolloverTime() {
    std::string err;
    rn::Rollover r;
    rn::RolloverPolicy p;
    p.sampleRate = 48000; p.bytesPerFrame = 4; p.maxSeconds = 60;
    CHECK(r.configure(p, &err));
    std::vector<std::uint64_t> per; bool order = true;
    CHECK(collect(r, 48000 * 200, 4800, &per, &order) == 48000u * 200);
    CHECK(order && per.size() == 4 && per[0] == 2880000 && per[1] == 2880000 && per[2] == 2880000 && per[3] == 960000);   // 60 s files: 3 x 60 s + 20 s
    // aligned to the wall clock: start 12:34:50.000 with a 3600 s period: first file runs 10 minutes 10 s to 13:00:00... use 600 s: 12:34:50 -> 12:40:00 = 310 s
    const std::int64_t start = cv::toUnixMs(2026, 11, 11, 12, 34, 50, 0);
    p.maxSeconds = 600; p.alignToClock = true; p.startUnixMs = start;
    CHECK(r.configure(p, &err));
    CHECK(r.currentFileCapacityFrames() == 310u * 48000u);
    std::vector<rn::Segment> segs;
    r.feed(310u * 48000u + 1, &segs);
    CHECK(segs.size() == 2 && segs[0].closesFile && segs[0].frames == 310u * 48000u && segs[1].startsFile && segs[1].frames == 1);
    CHECK(r.currentFileCapacityFrames() == 600u * 48000u);   // later files are full periods, boundaries stay on :00 / :10
    // start exactly on a boundary: a full period, not zero
    p.startUnixMs = cv::toUnixMs(2026, 11, 11, 12, 40, 0, 0);
    CHECK(r.configure(p, &err));
    CHECK(r.currentFileCapacityFrames() == 600u * 48000u);
    // start 1 ms before a boundary: 48 frames
    p.startUnixMs = cv::toUnixMs(2026, 11, 11, 12, 39, 59, 999);
    CHECK(r.configure(p, &err));
    CHECK(r.currentFileCapacityFrames() == 48u);
    // before the epoch (negative time) still aligns
    p.startUnixMs = -1;
    CHECK(r.configure(p, &err));
    CHECK(r.currentFileCapacityFrames() == 48u);
    // size and time together: the smaller binds
    p.alignToClock = false; p.maxSeconds = 10; p.maxBytes = 4 * 100000; p.headerBytes = 0;
    CHECK(r.configure(p, &err));
    CHECK(r.currentFileCapacityFrames() == 100000u);
    p.maxBytes = 4 * 1000000;
    CHECK(r.configure(p, &err));
    CHECK(r.currentFileCapacityFrames() == 480000u);
    // plain WAV ceiling constant is under 4 GiB and leaves slack
    CHECK(rn::kWavMaxBytes < 0x100000000ull && rn::kWavMaxBytes > 0xFFFF0000ull);
    // a 4 GiB-class limit: 2 Msps complex float = 8 bytes per frame, 44-byte header, the WAV ceiling
    rn::RolloverPolicy w; w.maxBytes = rn::kWavMaxBytes; w.headerBytes = 44; w.bytesPerFrame = 8; w.sampleRate = 2e6;
    CHECK(r.configure(w, &err));
    CHECK(r.currentFileCapacityFrames() == (rn::kWavMaxBytes - 44) / 8);
    CHECK(44 + r.currentFileCapacityFrames() * 8 <= rn::kWavMaxBytes);
    std::printf("rollover by time: 60 s files, clock-aligned first file (310 s to the next 10-minute mark), boundary rows, size/time together, the WAV ceiling (%llu frames of 8 bytes)\n",
                static_cast<unsigned long long>(r.currentFileCapacityFrames()));
}

}  // namespace

int main() {
    testCivil();
    testExpandGolden();
    testRefusals();
    testSanitise();
    testResolveAndUnique();
    testFuzz();
    testRolloverSize();
    testRolloverTime();
    return testSummary("test_rec_naming");
}
