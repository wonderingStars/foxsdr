// Tests for the power / SNR logging block (core/power_log.hpp).
//
// The Influx formatter is checked against hand-written golden lines AND against an independent parser written here from the line
// protocol reference (split on unescaped spaces and commas, backslash rules), fed random nasty tag values. The CSV logger runs on
// an in-memory FileSink so rotation, header, restart and disk refusal are deterministic; a real-file run covers the real sink.
// The senders talk only to stand-in listeners on the loopback interface inside this test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/power_log.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace {

namespace pl = cascade::core::powerlog;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kNs = 1000000000LL;

pl::Record rec(std::int64_t sec, int ms = 0, int radio = 0) {
    pl::Record r;
    r.unixNs = sec * kNs + static_cast<std::int64_t>(ms) * 1000000LL;
    r.radio = radio;
    r.frequencyHz = 14200000.0;
    r.bandwidthHz = 2400.0;
    r.signalDbfs = -45.678;
    r.noiseDbfs = -90.1;
    r.snrDb = 44.4;
    return r;
}

// ---- time, CSV line, SNR ----------------------------------------------------------------------------------------------------

void testTime() {
    CHECK(pl::isoUtc(0) == "1970-01-01T00:00:00.000Z");
    CHECK(pl::isoUtc(1000000000LL * kNs) == "2001-09-09T01:46:40.000Z");
    CHECK(pl::isoUtc(1234567890LL * kNs + 123000000LL) == "2009-02-13T23:31:30.123Z");
    CHECK(pl::isoUtc(1700000000LL * kNs) == "2023-11-14T22:13:20.000Z");
    CHECK(pl::isoUtc(951782400LL * kNs) == "2000-02-29T00:00:00.000Z");          // leap day, divisible by 400
    CHECK(pl::isoUtc(4107542400LL * kNs) == "2100-03-01T00:00:00.000Z");         // 2100 is not a leap year
    CHECK(pl::isoUtc(4107542399LL * kNs + 999000000LL) == "2100-02-28T23:59:59.999Z");
    CHECK(pl::isoUtc(-5) == "1970-01-01T00:00:00.000Z");                         // clamped
    CHECK(pl::dayStamp(1234567890LL * kNs) == "20090213");
    // against the standard library's calendar for many days
    std::mt19937_64 g(9);
    bool ok = true;
    for (int i = 0; i < 20000; ++i) {
        const std::int64_t days = static_cast<std::int64_t>(g() % 80000);   // 1970 .. 2189
        const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
        char want[16];
        std::snprintf(want, sizeof want, "%04d%02u%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        ok = ok && pl::dayStamp(days * 86400LL * kNs + static_cast<std::int64_t>(g() % 86400000000000ULL)) == want;
    }
    CHECK(ok);
}

void testCsvLine() {
    pl::Record r = rec(1234567890, 123);
    r.radio = 2;
    r.hasDbm = true;
    r.signalDbm = -80.5;
    CHECK(pl::csvLine(r) == "2009-02-13T23:31:30.123Z,2,14200000.0,2400.0,-45.68,-90.10,44.40,-80.50");
    r.hasDbm = false;
    CHECK(pl::csvLine(r) == "2009-02-13T23:31:30.123Z,2,14200000.0,2400.0,-45.68,-90.10,44.40,");
    r.noiseDbfs = NAN;
    r.snrDb = INFINITY;
    CHECK(pl::csvLine(r) == "2009-02-13T23:31:30.123Z,2,14200000.0,2400.0,-45.68,,,");
    // the header names the eight columns and every line has seven commas
    const std::string h = pl::csvHeader();
    CHECK(h == "time_utc,radio,freq_hz,bandwidth_hz,signal_dbfs,noise_dbfs,snr_db,signal_dbm");
    CHECK(std::count(h.begin(), h.end(), ',') == 7);
    bool cols = true;
    std::mt19937 g(3);
    for (int i = 0; i < 500; ++i) {
        pl::Record x = rec(1700000000 + i);
        if (g() % 3 == 0) { x.signalDbfs = NAN; }
        if (g() % 3 == 0) { x.snrDb = NAN; }
        x.hasDbm = g() % 2 == 0;
        x.signalDbm = x.hasDbm ? -70.0 - static_cast<double>(g() % 40) : NAN;
        const std::string l = pl::csvLine(x);
        cols = cols && std::count(l.begin(), l.end(), ',') == 7 && l.find('"') == std::string::npos;
    }
    CHECK(cols);
}

void testSnr() {
    CHECK_NEAR(pl::snrFromTotalAndFloor(-60.0, -70.0), 10.0 * std::log10(9.0), 1e-9);       // (S+N)/N = 10 dB -> S/N = 9
    CHECK_NEAR(pl::snrFromTotalAndFloor(-50.0, -90.0), 10.0 * std::log10(1e4 - 1.0), 1e-9);
    CHECK(std::isnan(pl::snrFromTotalAndFloor(-70.0, -70.0)));
    CHECK(std::isnan(pl::snrFromTotalAndFloor(-71.0, -70.0)));
    CHECK(std::isnan(pl::snrFromTotalAndFloor(-69.95, -70.0)));                               // under 0.1 dB above the floor
    CHECK(std::isnan(pl::snrFromTotalAndFloor(NAN, -70.0)) && std::isnan(pl::snrFromTotalAndFloor(-60.0, INFINITY)));
}

// ---- Influx line protocol ----------------------------------------------------------------------------------------------------

// An independent reader written from the reference: measurement[,tag=value...] SPACE field=value[,...] [SPACE timestamp]. Backslash escapes
// the next character in names, tag values and quoted string values; a field value is either a quoted string or runs to the next comma or space.
struct Parsed {
    std::string measurement;
    std::map<std::string, std::string> tags;
    std::vector<std::pair<std::string, std::string>> fields;   // key (decoded), raw value text (strings keep their quotes, decoded inside)
    std::string timestamp;
};
std::string unescape(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) { if (s[i] == '\\' && i + 1 < s.size()) { ++i; } o += s[i]; }
    return o;
}
bool parseLine(const std::string& line, Parsed* p) {
    std::size_t sp = std::string::npos;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++i; continue; }
        if (line[i] == ' ') { sp = i; break; }
    }
    if (sp == std::string::npos) { return false; }
    const std::string head = line.substr(0, sp);
    std::vector<std::string> hp(1);
    for (std::size_t i = 0; i < head.size(); ++i) {
        if (head[i] == '\\' && i + 1 < head.size()) { hp.back() += head[i]; hp.back() += head[++i]; continue; }
        if (head[i] == ',') { hp.emplace_back(); continue; }
        hp.back() += head[i];
    }
    p->measurement = unescape(hp[0]);
    for (std::size_t i = 1; i < hp.size(); ++i) {
        std::size_t eq = std::string::npos;
        for (std::size_t j = 0; j < hp[i].size(); ++j) {
            if (hp[i][j] == '\\') { ++j; continue; }
            if (hp[i][j] == '=') { eq = j; break; }
        }
        if (eq == std::string::npos) { return false; }
        p->tags[unescape(hp[i].substr(0, eq))] = unescape(hp[i].substr(eq + 1));
    }
    std::size_t i = sp + 1;
    while (i < line.size()) {
        std::string key;
        while (i < line.size() && line[i] != '=') {
            if (line[i] == '\\' && i + 1 < line.size()) { ++i; }
            key += line[i++];
        }
        if (i >= line.size()) { return false; }
        ++i;   // '='
        std::string val;
        if (i < line.size() && line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) { ++i; }
                val += line[i++];
            }
            if (i >= line.size()) { return false; }
            ++i;   // closing quote
            val = "\"" + val + "\"";
        } else {
            while (i < line.size() && line[i] != ',' && line[i] != ' ') { val += line[i++]; }
        }
        p->fields.emplace_back(key, val);
        if (i >= line.size()) { break; }
        if (line[i] == ',') { ++i; continue; }
        if (line[i] == ' ') { p->timestamp = line.substr(i + 1); break; }
        return false;
    }
    return true;
}

void testInfluxGolden() {
    pl::Record r = rec(0);
    r.unixNs = 1234567890123456789LL;       // more digits than a double holds: must survive as an integer
    r.radio = 1;
    r.frequencyHz = 14200000.4;
    r.bandwidthHz = 2400.0;
    r.signalDbfs = -45.5;
    r.noiseDbfs = -90.0;
    r.snrDb = 44.5;
    r.hasDbm = true;
    r.signalDbm = -80.0;
    pl::InfluxOptions o;
    o.tags = {{"host", "my host,1=x\\"}};
    o.stringFields = {{"mode", "U\"S\\B"}};
    const std::string want =
        "foxsdr_signal,radio=1,host=my\\ host\\,1\\=x\\\\ freq_hz=14200000i,bw_hz=2400i,signal_dbfs=-45.5,noise_dbfs=-90,snr_db=44.5,signal_dbm=-80,"
        "mode=\"U\\\"S\\\\B\" 1234567890123456789";
    CHECK(pl::influxLine(r, o) == want);
    if (pl::influxLine(r, o) != want) { std::printf("  got:  %s\n  want: %s\n", pl::influxLine(r, o).c_str(), want.c_str()); }
    // measurement escaping, leading underscore stripped, empty tag dropped, NaN omitted, no timestamp when 0, newline not representable
    pl::InfluxOptions o2;
    o2.measurement = "_foxsdr signal,x";
    o2.tags = {{"_band", "20 m"}, {"empty", ""}, {"__", "dropped because the key is empty"}, {"line", "a\nb"}};
    pl::Record r2 = rec(0);
    r2.unixNs = 0;
    r2.signalDbfs = NAN;
    r2.noiseDbfs = NAN;
    r2.snrDb = NAN;
    CHECK(pl::influxLine(r2, o2) == "foxsdr\\ signal\\,x,radio=0,band=20\\ m,line=a_b freq_hz=14200000i,bw_hz=2400i");
    // a string-field KEY is escaped once (the independent parser found it escaped twice: a b=c became a\\\ b\\\=c)
    pl::InfluxOptions o5;
    o5.stringFields = {{"a b=c", "v"}};
    pl::Record r5 = rec(0);
    r5.unixNs = 0;
    CHECK(pl::influxLine(r5, o5) == "foxsdr_signal,radio=0 freq_hz=14200000i,bw_hz=2400i,signal_dbfs=-45.678,noise_dbfs=-90.1,snr_db=44.4,a\\ b\\=c=\"v\"");
    // no field at all -> no line (a point needs a field)
    pl::Record r3;
    r3.frequencyHz = NAN;
    r3.bandwidthHz = NAN;
    CHECK(pl::influxLine(r3).empty());
    // a float that needs an exponent is still a legal float; an integer ends in i; a huge frequency does not overflow
    pl::Record r4 = rec(5);
    r4.signalDbfs = 1.25e-7;
    const std::string l4 = pl::influxLine(r4);
    CHECK(l4.find("signal_dbfs=1.25e-07") != std::string::npos);
    CHECK(l4.find("freq_hz=14200000i") != std::string::npos && l4.size() > 10 && l4.substr(l4.size() - 10) == "5000000000");
}

void testInfluxAgainstParser() {
    std::mt19937 g(2026);
    const char alphabet[] = {'a', 'b', ' ', ',', '=', '"', '\\', '_', 'x', '\n', 'Z', '9'};
    long checked = 0;
    bool allOk = true;
    for (int it = 0; it < 4000; ++it) {
        const auto randStr = [&](int maxLen) {
            std::string s;
            const int n = static_cast<int>(g() % static_cast<unsigned>(maxLen + 1));
            for (int i = 0; i < n; ++i) { s += alphabet[g() % sizeof alphabet]; }
            return s;
        };
        pl::InfluxOptions o;
        o.measurement = randStr(8);
        std::map<std::string, std::string> expectTags;
        for (int t = 0; t < 3; ++t) {
            const std::string k = randStr(6), v = randStr(8);
            o.tags.emplace_back(k, v);
        }
        o.stringFields = {{randStr(5), randStr(10)}};
        const pl::Record r = rec(1700000000, 0, static_cast<int>(g() % 8));
        const std::string line = pl::influxLine(r, o);
        ++checked;
        if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos) { allOk = false; continue; }
        Parsed p;
        if (!parseLine(line, &p)) { allOk = false; std::printf("  unparsable: %s\n", line.c_str()); continue; }
        if (p.timestamp != "1700000000000000000" || p.fields.size() < 5) { allOk = false; std::printf("  shape: %s\n", line.c_str()); continue; }
        // the radio tag and the surviving user tags decode to the originals (after the documented normalisation)
        const auto norm = [](std::string s, bool key) {
            for (auto& c : s) { if (c == '\n' || c == '\r') { c = '_'; } }
            if (key) { std::size_t i = 0; while (i < s.size() && s[i] == '_') { ++i; } s = s.substr(i); }
            return s;
        };
        {   // the string field decodes to its original (newlines become spaces), under its normalised key
            std::string k = o.stringFields[0].first, v = o.stringFields[0].second;
            for (auto& c : k) { if (c == '\n' || c == '\r') { c = '_'; } }
            for (auto& c : v) { if (c == '\n' || c == '\r') { c = ' '; } }
            std::size_t u = 0;
            while (u < k.size() && k[u] == '_') { ++u; }
            k = k.substr(u);
            bool found = k.empty();
            for (const auto& f : p.fields) { if (!k.empty() && f.first == k && f.second == "\"" + v + "\"") { found = true; } }
            if (!found) { allOk = false; std::printf("  string field lost: key [%s] val [%s] parsed last [%s] = [%s] orig [%s] line [%s]%c", k.c_str(), v.c_str(), p.fields.back().first.c_str(), p.fields.back().second.c_str(), o.stringFields[0].first.c_str(), line.c_str(), 10); }
        }
        if (p.tags["radio"] != std::to_string(r.radio)) { allOk = false; }
        std::map<std::string, std::string> want;
        for (const auto& t : o.tags) {
            const std::string k = norm(t.first, true), v = norm(t.second, false);
            if (!k.empty() && !v.empty()) { want[k] = v; }
        }
        std::map<std::string, std::string> got = p.tags;
        got.erase("radio");
        if (got != want) { allOk = false; std::printf("  tags differ for %s\n", line.c_str()); }
    }
    std::printf("  %ld random lines checked against the independent parser\n", checked);
    CHECK(allOk);
}

// ---- the CSV logger on an in-memory sink --------------------------------------------------------------------------------------------

struct Mem {
    std::mutex mu;
    std::map<std::string, std::string> files;
    std::vector<std::string> openOrder;
    std::thread::id writerThread;
    bool sawWriter = false;
    bool failOpen = false;
    long failWriteAfter = -1;      // fail every write once this many succeeded in total (-1: never)
    long writes = 0;
};

class MemSink final : public pl::FileSink {
public:
    explicit MemSink(Mem* m) : m_(m) {}
    bool open(const std::string& path, std::uint64_t* existing) override {
        std::lock_guard<std::mutex> lk(m_->mu);
        if (m_->failOpen) { return false; }
        cur_ = path;
        *existing = m_->files[path].size();
        m_->openOrder.push_back(path);
        return true;
    }
    bool write(const char* d, std::size_t n) override {
        std::lock_guard<std::mutex> lk(m_->mu);
        m_->writerThread = std::this_thread::get_id();
        m_->sawWriter = true;
        if (m_->failWriteAfter >= 0 && m_->writes >= m_->failWriteAfter) { return false; }
        ++m_->writes;
        m_->files[cur_].append(d, n);
        return true;
    }
    bool flush() override { return true; }
    void close() override {}

private:
    Mem* m_;
    std::string cur_;
};

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) { v.push_back(l); }
    return v;
}

void waitWritten(pl::CsvLogger& lg, std::uint64_t n) {
    for (int i = 0; i < 400 && lg.written() + lg.droppedDiskFailing() < n; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}

void testLoggerBasics() {
    // header first, stable order, interval per radio, thread, restart appends without a second header
    Mem m;
    pl::CsvOptions o;
    o.directory = "d";
    o.intervalS = 1.0;
    {
        pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
        lg.start();
        const std::thread::id me = std::this_thread::get_id();
        CHECK(lg.offer(rec(1000, 0, 0)));
        CHECK(!lg.offer(rec(1000, 500, 0)));          // within the interval of the same radio
        CHECK(lg.offer(rec(1000, 500, 1)));           // another radio has its own clock
        CHECK(lg.offer(rec(1001, 0, 0)));             // exactly one interval later
        CHECK(!lg.offer(rec(1001, 400, 0)));
        CHECK(lg.skippedByInterval() == 2 && lg.queued() == 3);
        waitWritten(lg, 3);
        lg.stop();
        CHECK(lg.written() == 3 && lg.writeFailures() == 0 && lg.diskHealthy());
        CHECK(m.sawWriter && m.writerThread != me);       // every byte went out on the worker thread
        CHECK(m.files.size() == 1);
        const std::string& f = m.files.begin()->second;
        const auto ls = lines(f);
        CHECK(ls.size() == 4 && ls[0] == pl::csvHeader());
        CHECK(ls[1].rfind("1970-01-01T00:16:40.000Z,0,", 0) == 0 && ls[2].rfind("1970-01-01T00:16:40.500Z,1,", 0) == 0 &&
              ls[3].rfind("1970-01-01T00:16:41.000Z,0,", 0) == 0);
        CHECK(m.files.begin()->first == "d/foxsdr-power-19700101.csv" || m.files.begin()->first == "d\\foxsdr-power-19700101.csv");
    }
    {   // a second run the same day appends and writes no second header
        pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
        lg.start();
        lg.offer(rec(1005, 0, 0));
        waitWritten(lg, 1);
        lg.stop();
        const auto ls = lines(m.files.begin()->second);
        long headers = 0;
        for (const auto& l : ls) { headers += (l == pl::csvHeader()); }
        CHECK(ls.size() == 5 && headers == 1);
    }
}

void testRotation() {
    // by day: a record just before and just after UTC midnight go to two files, each with its header
    {
        Mem m;
        pl::CsvOptions o;
        o.intervalS = 0.0;
        pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
        lg.start();
        const std::int64_t midnight = 1234483200;       // 2009-02-13T00:00:00Z
        lg.offer(rec(midnight - 1, 900));
        lg.offer(rec(midnight, 0));
        lg.offer(rec(midnight, 100));
        waitWritten(lg, 3);
        lg.stop();
        CHECK(m.files.size() == 2 && m.files.count("foxsdr-power-20090212.csv") == 1 && m.files.count("foxsdr-power-20090213.csv") == 1);
        CHECK(lines(m.files["foxsdr-power-20090212.csv"]).size() == 2 && lines(m.files["foxsdr-power-20090213.csv"]).size() == 3);
        CHECK(!lines(m.files["foxsdr-power-20090213.csv"]).empty() && lines(m.files["foxsdr-power-20090213.csv"])[0] == pl::csvHeader());
    }
    // by size: no file ever exceeds the limit, every file has a header, no line is lost
    {
        Mem m;
        pl::CsvOptions o;
        o.intervalS = 0.0;
        const std::size_t oneLine = pl::csvLine(rec(1700000000)).size() + 1;
        o.rotateBytes = std::string(pl::csvHeader()).size() + 1 + 3 * oneLine;       // header + exactly 3 lines
        pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
        lg.start();
        for (int i = 0; i < 10; ++i) { lg.offer(rec(1700000000 + i)); }
        waitWritten(lg, 10);
        lg.stop();
        std::size_t total = 0;
        bool sizes = true, headers = true;
        for (const auto& f : m.files) {
            sizes = sizes && f.second.size() <= o.rotateBytes;
            const auto ls = lines(f.second);
            headers = headers && !ls.empty() && ls[0] == pl::csvHeader();
            total += ls.size() - 1;
        }
        CHECK(m.files.size() == 4 && sizes && headers && total == 10);          // 3 + 3 + 3 + 1
        CHECK(m.files.count("foxsdr-power-20231114-001.csv") == 1 && m.files.count("foxsdr-power-20231114.csv") == 1);
    }
    // restart after a full file: the next part is used rather than overgrowing it
    {
        Mem m;
        pl::CsvOptions o;
        o.intervalS = 0.0;
        const std::size_t oneLine = pl::csvLine(rec(1700000000)).size() + 1;
        o.rotateBytes = std::string(pl::csvHeader()).size() + 1 + 2 * oneLine;
        for (int run = 0; run < 2; ++run) {
            pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
            lg.start();
            lg.offer(rec(1700000000 + run * 2));
            lg.offer(rec(1700000001 + run * 2));
            waitWritten(lg, 2);
            lg.stop();
        }
        CHECK(m.files.size() == 2 && lines(m.files["foxsdr-power-20231114.csv"]).size() == 3 && lines(m.files["foxsdr-power-20231114-001.csv"]).size() == 3);
    }
}

void testQueueAndAllocation() {
    // the queue is bounded; overflow is counted, never blocks; stop() drains what was queued
    Mem m;
    pl::CsvOptions o;
    o.intervalS = 0.0;
    o.queueCapacity = 16;
    pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
    long accepted = 0;
    for (int i = 0; i < 100; ++i) { accepted += lg.offer(rec(1700000000 + i)) ? 1 : 0; }     // worker not started: the ring fills
    CHECK(accepted == 16 && lg.queued() == 16 && lg.droppedQueueFull() == 84);
    lg.start();
    lg.stop();
    CHECK(lg.written() == 16);
    // offer() allocates nothing and is cheap
    pl::CsvLogger lg2(o, std::make_unique<MemSink>(&m));
    lg2.start();
    {
        AllocProbe probe;
        const auto t0 = Clock::now();
        for (int i = 0; i < 20000; ++i) { lg2.offer(rec(1700001000 + i)); }
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 20000.0;
        CHECK(probe.news() == 0);
        std::printf("  offer(): %.0f ns per call (includes a full or draining queue)\n", ns);
        CHECK(ns < 5000.0);
    }
    lg2.stop();
    CHECK(lg2.written() + lg2.droppedQueueFull() + lg2.droppedDiskFailing() == 20000);
    // stop() right after a burst loses nothing that was accepted
    pl::CsvOptions o3;
    o3.intervalS = 0.0;
    o3.queueCapacity = 4096;
    Mem m3;
    pl::CsvLogger lg3(o3, std::make_unique<MemSink>(&m3));
    lg3.start();
    for (int i = 0; i < 1000; ++i) { lg3.offer(rec(1700000000 + i)); }
    lg3.stop();
    CHECK(lg3.written() == lg3.queued() && lg3.queued() == 1000);
}

void testDiskRefusal() {
    Mem m;
    pl::CsvOptions o;
    o.intervalS = 0.0;
    o.retryMs = 60;
    m.failOpen = true;
    pl::CsvLogger lg(o, std::make_unique<MemSink>(&m));
    lg.start();
    for (int i = 0; i < 20; ++i) { lg.offer(rec(1700000000 + i)); }
    waitWritten(lg, 20);
    CHECK(lg.written() == 0 && lg.droppedDiskFailing() == 20 && lg.writeFailures() >= 1 && !lg.diskHealthy() && !lg.lastError().empty());
    CHECK(lg.writeFailures() < 20);                      // the retry interval spared the disk: not one attempt per record
    // the disk comes back: after the retry interval the logger resumes, with a header in the new file
    m.failOpen = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    for (int i = 0; i < 5; ++i) { lg.offer(rec(1700000100 + i)); }
    for (int i = 0; i < 400 && lg.written() < 5; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    lg.stop();
    CHECK(lg.written() == 5 && lg.diskHealthy());
    CHECK(m.files.size() == 1 && lines(m.files.begin()->second).size() == 6 && lines(m.files.begin()->second)[0] == pl::csvHeader());
    // a write that fails in the middle of a file: that record is dropped and counted, nothing throws, later records land
    Mem m2;
    m2.failWriteAfter = 4;       // header + 3 lines succeed, then the disk refuses
    pl::CsvOptions o2 = o;
    pl::CsvLogger lg2(o2, std::make_unique<MemSink>(&m2));
    lg2.start();
    for (int i = 0; i < 10; ++i) { lg2.offer(rec(1700000000 + i)); }
    waitWritten(lg2, 10);
    m2.failWriteAfter = -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    lg2.offer(rec(1700000200));
    for (int i = 0; i < 400 && lg2.written() < 4; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    lg2.stop();
    std::printf("  write refused after 3 lines: written %llu, dropped %llu, failures %llu\n", static_cast<unsigned long long>(lg2.written()),
                static_cast<unsigned long long>(lg2.droppedDiskFailing()), static_cast<unsigned long long>(lg2.writeFailures()));
    CHECK(lg2.written() >= 4 && lg2.droppedDiskFailing() >= 1 && lg2.writeFailures() >= 1 && lg2.diskHealthy());
}

void testRealFiles() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "blk2-powerlog";
    std::error_code ec;
    fs::remove_all(dir, ec);
    pl::CsvOptions o;
    o.directory = (dir / "sub").string();        // created on first write
    o.intervalS = 0.0;
    {
        pl::CsvLogger lg(o);
        lg.start();
        const std::int64_t midnight = 1234483200;
        lg.offer(rec(midnight - 1));
        lg.offer(rec(midnight + 5));
        waitWritten(lg, 2);
        lg.stop();
        CHECK(lg.written() == 2 && lg.diskHealthy());
    }
    {
        std::ifstream a(dir / "sub" / "foxsdr-power-20090212.csv"), b(dir / "sub" / "foxsdr-power-20090213.csv");
        std::string la, lb, l2;
        std::getline(a, la);
        std::getline(b, lb);
        std::getline(b, l2);
        CHECK(la == pl::csvHeader() && lb == pl::csvHeader() && l2.rfind("2009-02-13T00:00:05.000Z,0,", 0) == 0);
    }
    // the disk refuses: the target is a read-only file -> counted, never an exception, and the logger stays usable
    {
        pl::CsvOptions ro = o;
        ro.directory = (dir / "ro").string();
        fs::create_directories(dir / "ro", ec);
        {
            std::ofstream f(dir / "ro" / "foxsdr-power-20090213.csv");
            f << "x\n";
        }
        fs::permissions(dir / "ro" / "foxsdr-power-20090213.csv", fs::perms::owner_read, ec);
        ro.retryMs = 50;
        pl::CsvLogger lg(ro);
        lg.start();
        for (int i = 0; i < 5; ++i) { lg.offer(rec(1234483200 + i)); }
        waitWritten(lg, 5);
        lg.stop();
        fs::permissions(dir / "ro" / "foxsdr-power-20090213.csv", fs::perms::owner_all, ec);
        std::printf("  read-only target: written %llu, dropped %llu, failures %llu, last error '%s'\n", static_cast<unsigned long long>(lg.written()),
                    static_cast<unsigned long long>(lg.droppedDiskFailing()), static_cast<unsigned long long>(lg.writeFailures()), lg.lastError().c_str());
        CHECK(lg.written() == 0 && lg.droppedDiskFailing() == 5 && lg.writeFailures() >= 1 && !lg.diskHealthy());
    }
    // a directory path that is really a file
    {
        pl::CsvOptions bad = o;
        {
            std::ofstream f(dir / "notadir");
            f << "x";
        }
        bad.directory = (dir / "notadir").string();
        pl::CsvLogger lg(bad);
        lg.start();
        lg.offer(rec(1234483200));
        waitWritten(lg, 1);
        lg.stop();
        CHECK(lg.written() == 0 && lg.writeFailures() >= 1);
    }
    fs::remove_all(dir, ec);
}

// ---- stand-in listeners and the senders ---------------------------------------------------------------------------------------------------

namespace net = pl::detail;

class UdpStub {
public:
    UdpStub() {
        net::ensureNet();
        s_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        ::getsockname(s_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
#if defined(_WIN32)
        DWORD ms = 300;
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#endif
    }
    ~UdpStub() { net::closeSock(s_); }
    int port() const { return port_; }
    // Collects datagrams until none arrives for the receive timeout.
    std::vector<std::string> drain() {
        std::vector<std::string> v;
        char buf[70000];
        while (true) {
            const int n = ::recv(s_, buf, sizeof buf, 0);
            if (n <= 0) { break; }
            v.emplace_back(buf, static_cast<std::size_t>(n));
        }
        return v;
    }

private:
    net::sock_t s_;
    int port_ = 0;
};

class TcpStub {
public:
    enum class Mode { Reply204, Reply401, CloseSilently, NeverReply };
    explicit TcpStub(Mode m) : mode_(m) {
        net::ensureNet();
        l_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(l_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(l_, 4);
        socklen_t len = sizeof a;
        ::getsockname(l_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] { serve(); });
    }
    ~TcpStub() {
        stop_ = true;
        net::closeSock(l_);
        if (th_.joinable()) { th_.join(); }
    }
    int port() const { return port_; }
    std::string request() {
        std::lock_guard<std::mutex> lk(mu_);
        return req_;
    }

private:
    void serve() {
        while (!stop_) {
            const net::sock_t c = ::accept(l_, nullptr, nullptr);
            if (net::badSock(c)) { return; }
            std::string req;
            char buf[4096];
            std::size_t need = std::string::npos;
            while (true) {
                const int n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) { break; }
                req.append(buf, static_cast<std::size_t>(n));
                const std::size_t he = req.find("\r\n\r\n");
                if (he != std::string::npos) {
                    const std::size_t cl = req.find("Content-Length: ");
                    const std::size_t len = cl == std::string::npos ? 0 : static_cast<std::size_t>(std::atoi(req.c_str() + cl + 16));
                    need = he + 4 + len;
                }
                if (need != std::string::npos && req.size() >= need) { break; }
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                req_ = req;
            }
            if (mode_ == Mode::Reply204) { const char* r = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"; ::send(c, r, static_cast<int>(std::strlen(r)), 0); }
            if (mode_ == Mode::Reply401) { const char* r = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; ::send(c, r, static_cast<int>(std::strlen(r)), 0); }
            if (mode_ == Mode::NeverReply) { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); }
            net::closeSock(c);
        }
    }
    Mode mode_;
    net::sock_t l_;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::mutex mu_;
    std::string req_;
};

void testSenders() {
    // off by default: nothing is sent, nothing is touched
    {
        UdpStub u;
        pl::InfluxTarget t;                       // enabled == false
        CHECK(!t.enabled);
        t.host = "127.0.0.1";
        t.port = u.port();
        const pl::SendResult r = pl::sendUdp(t, {"a b=1"});
        CHECK(!r.ok && r.error == "disabled");
        CHECK(u.drain().empty());
        TcpStub h(TcpStub::Mode::Reply204);
        t.http = true;
        t.port = h.port();
        const pl::SendResult hr = pl::sendHttp(t, {"a b=1"});
        CHECK(!hr.ok && hr.error == "disabled" && h.request().empty());
    }
    // UDP: lines are grouped into datagrams of at most 1400 bytes, in order, none lost
    {
        UdpStub u;
        pl::InfluxTarget t;
        t.enabled = true;
        t.host = "127.0.0.1";
        t.port = u.port();
        std::vector<std::string> ls;
        for (int i = 0; i < 200; ++i) { ls.push_back(pl::influxLine(rec(1700000000 + i), pl::InfluxOptions())); }
        const pl::SendResult r = pl::sendUdp(t, ls);
        CHECK(r.ok);
        const auto got = u.drain();
        std::vector<std::string> all;
        bool sizes = true;
        for (const auto& d : got) {
            sizes = sizes && d.size() <= 1400;
            for (const auto& l : lines(d)) { all.push_back(l); }
        }
        std::printf("  UDP: %zu lines in %zu datagrams\n", ls.size(), got.size());
        CHECK(sizes && got.size() > 1 && all == ls);
        const auto one = pl::splitDatagrams({"aaa", "bbb", "ccc"}, 7);
        CHECK(one.size() == 2 && one[0] == "aaa\nbbb" && one[1] == "ccc");
        CHECK(pl::splitDatagrams({std::string(2000, 'x')}, 1400).size() == 1);      // an oversize line goes alone
        // bad targets
        pl::InfluxTarget bad = t;
        bad.port = 0;
        CHECK(!pl::sendUdp(bad, {"x"}).ok);
        bad = t;
        bad.host = "127.0.0.1\r\nX: y";
        CHECK(!pl::sendUdp(bad, {"x"}).ok);
    }
    // HTTP: the request is exactly what buildHttpPost makes; 204 is ok, 401 is not, silence and a refused port are errors
    {
        pl::InfluxTarget t;
        t.enabled = true;
        t.http = true;
        t.host = "127.0.0.1";
        t.org = "my org";
        t.bucket = "rf/signal";
        t.token = "SECRET-TOKEN-123";
        t.timeoutMs = 400;
        {
            TcpStub s(TcpStub::Mode::Reply204);
            t.port = s.port();
            const std::vector<std::string> ls = {"m,radio=0 v=1 1", "m,radio=1 v=2 2"};
            const pl::SendResult r = pl::sendHttp(t, ls);
            CHECK(r.ok && r.httpStatus == 204);
            const std::string body = "m,radio=0 v=1 1\nm,radio=1 v=2 2\n";
            CHECK(s.request() == pl::buildHttpPost(t, body));
            const std::string req = s.request();
            CHECK(req.rfind("POST /api/v2/write?precision=ns&org=my%20org&bucket=rf%2Fsignal HTTP/1.1\r\nHost: 127.0.0.1:", 0) == 0);
            CHECK(req.find("\r\nAuthorization: Token SECRET-TOKEN-123\r\n") != std::string::npos);
            CHECK(req.find("\r\nContent-Length: " + std::to_string(body.size()) + "\r\n") != std::string::npos);
            CHECK(req.size() >= body.size() && req.substr(req.size() - body.size()) == body);
            CHECK(r.error.find("SECRET") == std::string::npos);
        }
        {
            TcpStub s(TcpStub::Mode::Reply401);
            t.port = s.port();
            const pl::SendResult r = pl::sendHttp(t, {"m v=1"});
            CHECK(!r.ok && r.httpStatus == 401 && r.error == "HTTP 401" && r.error.find("SECRET") == std::string::npos);
        }
        {
            TcpStub s(TcpStub::Mode::CloseSilently);
            t.port = s.port();
            const pl::SendResult r = pl::sendHttp(t, {"m v=1"});
            CHECK(!r.ok && r.error == "no HTTP answer");
        }
        {
            TcpStub s(TcpStub::Mode::NeverReply);
            t.port = s.port();
            const auto t0 = Clock::now();
            const pl::SendResult r = pl::sendHttp(t, {"m v=1"});
            const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
            std::printf("  HTTP against a stand-in that never answers: returned after %.2f s with '%s'\n", sec, r.error.c_str());
            CHECK(!r.ok && sec < 1.2);
        }
        {
            int closedPort = 0;
            { UdpStub probe; closedPort = probe.port(); }       // a loopback port nothing listens on for TCP
            t.port = closedPort;
            const pl::SendResult r = pl::sendHttp(t, {"m v=1"});
            CHECK(!r.ok && !r.error.empty() && r.error.find("SECRET") == std::string::npos);
        }
        // header injection through the token or the path is refused before anything is sent: against a live listener, so a refusal cannot be mistaken for a refused port
        TcpStub live(TcpStub::Mode::Reply204);
        t.port = live.port();
        pl::InfluxTarget inj = t;
        inj.token = "x\r\nX-Evil: 1";
        CHECK(!pl::sendHttp(inj, {"m v=1"}).ok);
        inj = t;
        inj.path = "/a\r\nHost: evil";
        CHECK(!pl::sendHttp(inj, {"m v=1"}).ok);
        CHECK(live.request().empty());      // nothing reached a LIVE listener
        // no token -> no Authorization header
        pl::InfluxTarget nt = t;
        nt.token.clear();
        CHECK(pl::buildHttpPost(nt, "b").find("Authorization") == std::string::npos);
    }
}

}  // namespace

int main() {
    std::printf("time and CSV:\n");
    testTime();
    testCsvLine();
    testSnr();
    std::printf("influx formatter:\n");
    testInfluxGolden();
    testInfluxAgainstParser();
    std::printf("logger:\n");
    testLoggerBasics();
    testRotation();
    testQueueAndAllocation();
    testDiskRefusal();
    testRealFiles();
    std::printf("senders:\n");
    testSenders();
    return testSummary("test_power_log");
}
